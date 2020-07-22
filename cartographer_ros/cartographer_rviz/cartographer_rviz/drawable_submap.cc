/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer_rviz/drawable_submap.h"

#include <chrono>
#include <future>
#include <sstream>
#include <string>

#include "Eigen/Core"
#include "Eigen/Geometry"
#include "absl/memory/memory.h"
#include "cartographer/common/port.h"
#include "cartographer_ros/msg_conversion.h"
#include "cartographer_ros_msgs/SubmapQuery.h"
#include "eigen_conversions/eigen_msg.h"
#include "ros/ros.h"

namespace cartographer_rviz {

namespace {

constexpr std::chrono::milliseconds kMinQueryDelayInMs(250);
constexpr float kAlphaUpdateThreshold = 0.2f;

const Ogre::ColourValue kSubmapIdColor(Ogre::ColourValue::Red);
const Eigen::Vector3d kSubmapIdPosition(0.0, 0.0, 0.3);
constexpr float kSubmapIdCharHeight = 0.2f;
constexpr int kNumberOfSlicesPerSubmap = 2;

}  // namespace

DrawableSubmap::DrawableSubmap(const ::cartographer::mapping::SubmapId& id,
                               ::rviz::DisplayContext* const display_context,
                               Ogre::SceneNode* const map_node,
                               ::rviz::Property* const submap_category,
                               const bool visible, const bool pose_axes_visible,
                               const float pose_axes_length,
                               const float pose_axes_radius)
    : id_(id),
      display_context_(display_context),
      submap_node_(map_node->createChildSceneNode()),
      submap_id_text_node_(submap_node_->createChildSceneNode()),
      pose_axes_(display_context->getSceneManager(), submap_node_,
                 pose_axes_length, pose_axes_radius),
      pose_axes_visible_(pose_axes_visible),
      submap_id_text_(QString("(%1,%2)")
                          .arg(id.trajectory_id)
                          .arg(id.submap_index)
                          .toStdString()),
      last_query_timestamp_(0) {
  for (int slice_index = 0; slice_index < kNumberOfSlicesPerSubmap;
       ++slice_index) {
    ogre_slices_.emplace_back(absl::make_unique<OgreSlice>(
        id, slice_index, display_context->getSceneManager(), submap_node_,
        slice_index == 2));
  }
  // DrawableSubmap creates and manages its visibility property object
  // (a unique_ptr is needed because the Qt parent of the visibility
  // property is the submap_category object - the BoolProperty needs
  // to be destroyed along with the DrawableSubmap)
  visibility_ = absl::make_unique<::rviz::BoolProperty>(
      "" /* title */, visible, "" /* description */, submap_category,
      SLOT(ToggleVisibility()), this);
  submap_id_text_.setCharacterHeight(kSubmapIdCharHeight);
  submap_id_text_.setColor(kSubmapIdColor);
  submap_id_text_.setTextAlignment(::rviz::MovableText::H_CENTER,
                                   ::rviz::MovableText::V_ABOVE);
  submap_id_text_node_->setPosition(ToOgre(kSubmapIdPosition));
  submap_id_text_node_->attachObject(&submap_id_text_);
  TogglePoseMarkerVisibility();
  connect(this, SIGNAL(RequestSucceeded()), this, SLOT(UpdateSceneNode()));
}

DrawableSubmap::~DrawableSubmap() {
  // 'query_in_progress_' must be true until the Q_EMIT has happened. Qt then
  // makes sure that 'RequestSucceeded' is not called after our destruction.
  if (QueryInProgress()) {
    rpc_request_future_.wait();
  }
  display_context_->getSceneManager()->destroySceneNode(submap_node_);
  display_context_->getSceneManager()->destroySceneNode(submap_id_text_node_);
}

void DrawableSubmap::Update(
    const ::std_msgs::Header& header,
    const ::cartographer_ros_msgs::SubmapEntry& metadata) {
  absl::MutexLock locker(&mutex_);
  metadata_version_ = metadata.submap_version;
  pose_ = ::cartographer_ros::ToRigid3d(metadata.pose);
  submap_node_->setPosition(ToOgre(pose_.translation()));
  submap_node_->setOrientation(ToOgre(pose_.rotation()));
  display_context_->queueRender();
  visibility_->setName(
      QString("%1.%2").arg(id_.submap_index).arg(metadata_version_));
  visibility_->setDescription(
      QString("Toggle visibility of this individual submap.<br><br>"
              "Trajectory %1, submap %2, submap version %3")
          .arg(id_.trajectory_id)
          .arg(id_.submap_index)
          .arg(metadata_version_));
}

inline Eigen::Array2i FromFlatIndex(
    const int flat_index, const cartographer::io::SubmapTexture& texture) {
  return {flat_index / texture.width, flat_index % texture.width};
}

bool DrawableSubmap::MaybeFetchTexture(ros::ServiceClient* const client) {
  absl::MutexLock locker(&mutex_);
  // Received metadata version can also be lower if we restarted Cartographer.
  const bool newer_version_available =
      submap_textures_ == nullptr ||
      submap_textures_->version != metadata_version_;
  const std::chrono::milliseconds now =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch());
  const bool recently_queried =
      last_query_timestamp_ + kMinQueryDelayInMs > now;
  if (/*!newer_version_available || */recently_queried || query_in_progress_) {
    return false;
  }
  query_in_progress_ = true;
  last_query_timestamp_ = now;
  rpc_request_future_ = std::async(std::launch::async, [this, client]() {
    std::shared_ptr<::cartographer::io::SubmapTextures> submap_textures =
        ::cartographer_ros::FetchSubmapTextures(id_, client, &special_color_);
    absl::MutexLock locker(&mutex_);
    query_in_progress_ = false;
    if (submap_textures != nullptr) {
      // We emit a signal to update in the right thread, and pass via the
      // 'submap_texture_' member to simplify the signal-slot connection
      // slightly.
      if (submap_textures->textures.size() == 1) {
        auto& new_texture = submap_textures;
        auto texture_filtered =
            cartographer::io::SubmapTexture(new_texture->textures.at(0));
        //auto frontier_texture =
        //    cartographer::io::SubmapTexture(new_texture->textures.at(0));

        for (int i = 0; i < texture_filtered.pixels.intensity.size(); i++) {
          char& intensity = texture_filtered.pixels.intensity.at(i);
          char& alpha = texture_filtered.pixels.alpha.at(i);
          // unsigned char add = std::min((unsigned int)255, (unsigned
          // int)(submap_texture.pixels.intensity[i]) + (unsigned
          // int)(submap_texture.pixels.alpha[i]));
          if (alpha > 0) {
            alpha = 255;
          }
          if (intensity > 0) {
            if (intensity < 15)
              intensity = 0;
            else
              intensity = 255;
          }
        }
        /*const int w = texture_filtered.width;
        const int h = texture_filtered.height;
        auto is_unknown = [&texture_filtered](int index) {
          return texture_filtered.pixels.intensity.at(index) == 0 &&
              texture_filtered.pixels.alpha.at(index) == 0;
        };
        auto is_free = [&texture_filtered](int index) {
          return texture_filtered.pixels.intensity.at(index) == (char)255;
        };
        auto is_in_limits = [&texture_filtered](int index) {
          return (index >= 0) && (index < texture_filtered.pixels.intensity.size());
        };
        std::vector<std::pair<Eigen::Array2i,
                              std::unique_ptr<cartographer::mapping::SubmapId>>>
            submap_frontier_cells;
        for (int i = 0; i < texture_filtered.pixels.intensity.size(); i++) {
          frontier_texture.pixels.intensity.at(i) = 0;
          frontier_texture.pixels.alpha.at(i) = 0;
          if (is_unknown(i)) {
            int free_neighbours = 0;
            int unknown_neighbours = 0;
            auto check_neighbour = [&](int index) {
              if (is_in_limits(index) && is_unknown(index)) unknown_neighbours++;
              if (is_in_limits(index) && is_free(index)) free_neighbours++;
            };
            check_neighbour(i + 1);
            check_neighbour(i - 1);
            check_neighbour(i + w);
            check_neighbour(i + w + 1);
            check_neighbour(i + w - 1);
            check_neighbour(i - w);
            check_neighbour(i - w + 1);
            check_neighbour(i - w - 1);
            if (free_neighbours >= 3 && unknown_neighbours >= 3) {
              frontier_texture.pixels.intensity.at(i) = 255;
              frontier_texture.pixels.alpha.at(i) = 255;
              submap_frontier_cells.push_back(
                  std::make_pair(FromFlatIndex(i, frontier_texture), nullptr));
            }
          }
        }*/
          new_texture->textures.push_back(texture_filtered);
          //new_texture->textures.push_back(frontier_texture);
      }

      submap_textures_ = std::move(submap_textures);
      Q_EMIT RequestSucceeded();
    }
  });
  return true;
}

bool DrawableSubmap::QueryInProgress() {
  absl::MutexLock locker(&mutex_);
  return query_in_progress_;
}

void DrawableSubmap::SetAlpha(const double current_tracking_z,
                              const float fade_out_start_distance_in_meters) {
  const float fade_out_distance_in_meters =
      2.f * fade_out_start_distance_in_meters;
  const double distance_z =
      std::abs(pose_.translation().z() - current_tracking_z);
  const double fade_distance =
      std::max(distance_z - fade_out_start_distance_in_meters, 0.);
  const float target_alpha = static_cast<float>(
      std::max(0., 1. - fade_distance / fade_out_distance_in_meters));

  if (std::abs(target_alpha - current_alpha_) > kAlphaUpdateThreshold ||
      target_alpha == 0.f || target_alpha == 1.f) {
    current_alpha_ = target_alpha;
  }
  for (auto& slice : ogre_slices_) {
    slice->SetAlpha(current_alpha_);
  }
  display_context_->queueRender();
}

void DrawableSubmap::SetSliceVisibility(size_t slice_index, bool visible) {
  ogre_slices_.at(slice_index)->SetVisibility(visible);
  ToggleVisibility();
}

void DrawableSubmap::UpdateSceneNode() {
  absl::MutexLock locker(&mutex_);
  // std::array<double, 3> special_color = frontier_ ? 0.0f : 1.0f;
  for (size_t slice_index = 0; slice_index < ogre_slices_.size() &&
                               slice_index < submap_textures_->textures.size();
       ++slice_index) {
    ogre_slices_[slice_index]->Update(submap_textures_->textures[slice_index], special_color_);
  }
  display_context_->queueRender();
}

void DrawableSubmap::ToggleVisibility() {
  for (auto& ogre_slice : ogre_slices_) {
    ogre_slice->UpdateOgreNodeVisibility(visibility_->getBool());
  }
  display_context_->queueRender();
}

void DrawableSubmap::TogglePoseMarkerVisibility() {
  submap_id_text_node_->setVisible(pose_axes_visible_);
  pose_axes_.getSceneNode()->setVisible(pose_axes_visible_);
}

}  // namespace cartographer_rviz
