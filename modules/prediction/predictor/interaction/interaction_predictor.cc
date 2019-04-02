/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/prediction/predictor/interaction/interaction_predictor.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>

#include "modules/common/adapters/proto/adapter_config.pb.h"
#include "modules/prediction/common/prediction_gflags.h"
#include "modules/prediction/common/prediction_util.h"
#include "modules/prediction/container/adc_trajectory/adc_trajectory_container.h"
#include "modules/prediction/container/container_manager.h"

namespace apollo {
namespace prediction {

using apollo::common::PathPoint;
using apollo::common::Point3D;
using apollo::common::TrajectoryPoint;
using apollo::common::adapter::AdapterConfig;
using apollo::hdmap::LaneInfo;
using apollo::prediction::math_util::GetSByConstantAcceleration;

InteractionPredictor::InteractionPredictor() {
  BuildADCTrajectory(FLAGS_collision_cost_time_resolution);
}

void InteractionPredictor::Predict(Obstacle* obstacle) {
  Clear();

  CHECK_NOTNULL(obstacle);
  CHECK_GT(obstacle->history_size(), 0);

  Feature* feature_ptr = obstacle->mutable_latest_feature();

  if (!feature_ptr->has_lane() || !feature_ptr->lane().has_lane_graph()) {
    AERROR << "Obstacle [" << obstacle->id() << "] has no lane graph.";
    return;
  }

  std::vector<double> candidate_lon_accelerations =
      {0.0, -0.5, -1.0, -1.5, -2.0, -2.5, -3.0};
  double best_lon_acceleration = 0.0;
  double smallest_cost = std::numeric_limits<double>::max();
  std::vector<double> posteriors;
  double posterior_sum = 0.0;
  for (const LaneSequence& lane_sequence :
       feature_ptr->lane().lane_graph().lane_sequence()) {
    for (const double lon_acceleration : candidate_lon_accelerations) {
      double cost =
          ComputeTrajectoryCost(*obstacle, lane_sequence, lon_acceleration);
      if (cost < smallest_cost) {
        smallest_cost = cost;
        best_lon_acceleration = lon_acceleration;
      }
    }

    double likelihood = ComputeLikelihood(smallest_cost);
    double prior = lane_sequence.probability();
    double posterior = ComputePosterior(prior, likelihood);
    posteriors.push_back(posterior);
    posterior_sum += posterior;
  }

  int best_seq_idx = 0;
  double largest_posterior = 0.0;
  CHECK_EQ(posteriors.size(),
           feature_ptr->lane().lane_graph().lane_sequence_size());
  for (int i = 0; i < feature_ptr->lane().lane_graph().lane_sequence_size();
       ++i) {
    double normalized_posterior =
        posteriors[i] / (posterior_sum + FLAGS_double_precision);
    feature_ptr->mutable_lane()
        ->mutable_lane_graph()
        ->mutable_lane_sequence(i)
        ->set_probability(normalized_posterior);
    if (normalized_posterior > largest_posterior) {
      largest_posterior = normalized_posterior;
      best_seq_idx = i;
    }
  }

  double probability_threshold = 0.5;
  if (largest_posterior > probability_threshold) {
    for (const LaneSequence& lane_sequence :
         feature_ptr->lane().lane_graph().lane_sequence()) {
      if (lane_sequence.probability() < probability_threshold) {
        continue;
      }
      std::vector<TrajectoryPoint> points;
      DrawTrajectory(*obstacle, lane_sequence,
        best_lon_acceleration,
        FLAGS_prediction_trajectory_time_length,
        FLAGS_prediction_trajectory_time_resolution,
        &points);
    Trajectory trajectory = GenerateTrajectory(points);
    trajectories_.push_back(std::move(trajectory));
    }
  } else {
    std::vector<TrajectoryPoint> points;
    DrawTrajectory(*obstacle,
        feature_ptr->lane().lane_graph().lane_sequence(best_seq_idx),
        best_lon_acceleration,
        FLAGS_prediction_trajectory_time_length,
        FLAGS_prediction_trajectory_time_resolution,
        &points);
    Trajectory trajectory = GenerateTrajectory(points);
    trajectories_.push_back(std::move(trajectory));
  }
}

void InteractionPredictor::Clear() { Predictor::Clear(); }

void InteractionPredictor::BuildADCTrajectory(const double time_resolution) {
  auto adc_trajectory_container =
      ContainerManager::Instance()->GetContainer<ADCTrajectoryContainer>(
          AdapterConfig::PLANNING_TRAJECTORY);
  if (adc_trajectory_container == nullptr) {
    AERROR << "Null adc trajectory container";
    return;
  }
  const auto& adc_trajectory = adc_trajectory_container->adc_trajectory();
  double curr_timestamp = 0.0;
  for (const TrajectoryPoint& point : adc_trajectory.trajectory_point()) {
    if (point.relative_time() + FLAGS_double_precision > curr_timestamp) {
      adc_trajectory_.push_back(point);
      curr_timestamp += time_resolution;
    }
  }
}

bool InteractionPredictor::DrawTrajectory(
    const Obstacle& obstacle,
    const LaneSequence& lane_sequence,
    const double lon_acceleration,
    const double total_time, const double period,
    std::vector<TrajectoryPoint>* trajectory_points) {
  // Sanity check.
  CHECK_NOTNULL(trajectory_points);
  trajectory_points->clear();
  const Feature& feature = obstacle.latest_feature();
  if (!feature.has_position() || !feature.has_velocity() ||
      !feature.position().has_x() || !feature.position().has_y()) {
    AERROR << "Obstacle [" << obstacle.id()
           << " is missing position or velocity";
    return false;
  }

  Eigen::Vector2d position(feature.position().x(), feature.position().y());
  double speed = feature.speed();

  int lane_segment_index = 0;
  std::string lane_id =
      lane_sequence.lane_segment(lane_segment_index).lane_id();
  std::shared_ptr<const LaneInfo> lane_info = PredictionMap::LaneById(lane_id);
  double lane_s = 0.0;
  double lane_l = 0.0;
  if (!PredictionMap::GetProjection(position, lane_info, &lane_s, &lane_l)) {
    AERROR << "Failed in getting lane s and lane l";
    return false;
  }
  double approach_rate = FLAGS_go_approach_rate;
  if (!lane_sequence.vehicle_on_lane()) {
    approach_rate = FLAGS_cutin_approach_rate;
  }
  size_t total_num = static_cast<size_t>(total_time / period);
  for (size_t i = 0; i < total_num; ++i) {
    double relative_time = static_cast<double>(i) * period;
    Eigen::Vector2d point;
    double theta = M_PI;
    if (!PredictionMap::SmoothPointFromLane(lane_id, lane_s, lane_l, &point,
                                            &theta)) {
      AERROR << "Unable to get smooth point from lane [" << lane_id
             << "] with s [" << lane_s << "] and l [" << lane_l << "]";
      break;
    }
    TrajectoryPoint trajectory_point;
    PathPoint path_point;
    path_point.set_x(point.x());
    path_point.set_y(point.y());
    path_point.set_z(0.0);
    path_point.set_theta(theta);
    path_point.set_lane_id(lane_id);
    trajectory_point.mutable_path_point()->CopyFrom(path_point);
    trajectory_point.set_v(speed);
    trajectory_point.set_a(lon_acceleration);
    trajectory_point.set_relative_time(relative_time);
    trajectory_points->emplace_back(std::move(trajectory_point));

    lane_s += std::max(
        0.0, speed * period + 0.5 * lon_acceleration * period * period);
    speed += lon_acceleration * period;

    while (lane_s > PredictionMap::LaneById(lane_id)->total_length() &&
           lane_segment_index + 1 < lane_sequence.lane_segment_size()) {
      lane_segment_index += 1;
      lane_s = lane_s - PredictionMap::LaneById(lane_id)->total_length();
      lane_id = lane_sequence.lane_segment(lane_segment_index).lane_id();
    }

    lane_l *= approach_rate;
  }

  return true;
}

double InteractionPredictor::ComputeTrajectoryCost(const Obstacle& obstacle,
    const LaneSequence& lane_sequence, const double acceleration) {
  CHECK_GT(obstacle.history_size(), 0);
  double centri_acc_weight = FLAGS_centripedal_acceleration_cost_weight;
  double collision_weight = FLAGS_collision_cost_weight;
  double speed = obstacle.latest_feature().speed();
  double total_cost = 0.0;
  double centri_acc_cost =
      CentripetalAccelerationCost(lane_sequence, speed, acceleration);
  total_cost += centri_acc_weight * centri_acc_cost;
  if (LowerRightOfWayThanEgo(obstacle, lane_sequence)) {
    double collision_cost =
        CollisionWithEgoVehicleCost(lane_sequence, speed, acceleration);
    total_cost += collision_weight * collision_cost;
  }
  return total_cost;
}

double InteractionPredictor::CentripetalAccelerationCost(
    const LaneSequence& lane_sequence,
    const double speed, const double acceleration) {
  double cost_abs_sum = 0.0;
  double cost_sqr_sum = 0.0;
  double curr_time = 0.0;
  while (curr_time < FLAGS_prediction_trajectory_time_length) {
    double s = GetSByConstantAcceleration(speed, acceleration, curr_time);
    double v = std::max(0.0, speed + acceleration * curr_time);
    double kappa = GetLaneSequenceCurvatureByS(lane_sequence, s);
    double centri_acc = v * v * kappa;
    cost_abs_sum += std::abs(centri_acc);
    cost_sqr_sum += centri_acc * centri_acc;
    curr_time += FLAGS_collision_cost_time_resolution;
  }
  return cost_sqr_sum / (cost_abs_sum + FLAGS_double_precision);
}

double InteractionPredictor::CollisionWithEgoVehicleCost(
    const LaneSequence& lane_sequence,
    const double speed, const double acceleration) {
  double cost_abs_sum = 0.0;
  double cost_sqr_sum = 0.0;
  for (const TrajectoryPoint& adc_trajectory_point : adc_trajectory_) {
    double relative_time = adc_trajectory_point.relative_time();
    double s = GetSByConstantAcceleration(speed, acceleration, relative_time);
    Point3D position = GetPositionByLaneSequenceS(lane_sequence, s);
    double pos_x = position.x();
    double pos_y = position.y();
    double adc_x = adc_trajectory_point.path_point().x();
    double adc_y = adc_trajectory_point.path_point().y();
    double distance = std::hypot(adc_x - pos_x, adc_y - pos_y);
    double cost =
        std::exp(-FLAGS_collision_cost_exp_coefficient * distance * distance);
    cost_abs_sum += std::abs(cost);
    cost_sqr_sum += cost * cost;
  }
  return cost_sqr_sum / (cost_abs_sum + FLAGS_double_precision);
}

bool InteractionPredictor::LowerRightOfWayThanEgo(
    const Obstacle& obstacle, const LaneSequence& lane_sequence) {
  return lane_sequence.right_of_way() < 0;
}

double InteractionPredictor::ComputeLikelihood(const double cost) {
  double alpha = FLAGS_likelihood_exp_coefficient;
  return std::exp(-alpha * cost);
}

double InteractionPredictor::ComputePosterior(const double prior,
                                              const double likelihood) {
  return prior * likelihood;
}

}  // namespace prediction
}  // namespace apollo