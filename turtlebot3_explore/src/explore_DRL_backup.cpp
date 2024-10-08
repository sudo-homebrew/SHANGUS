/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2008, Robert Bosch LLC.
 *  Copyright (c) 2015-2016, Jiri Horner.
 *  Copyright (c) 2021, Carlos Alvarez, Juan Galvis.
 *  All rights reserved.
 *
 *  Modified by: Seunghyeop
 *  Description: This code has been modified to train the Turtlebot3 Waffle_pi model.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Jiri Horner nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *********************************************************************/

#include <explore/explore.h>

#include <thread>
#include <unistd.h>
#include <geometry_msgs/msg/pose.hpp>
#include "turtlebot3_msgs/srv/ring_goal.hpp"

#include "geometry_msgs/msg/twist.hpp"

#include <stdio.h>
#include <iostream>

inline static bool same_point(const geometry_msgs::msg::Point& one,
                              const geometry_msgs::msg::Point& two)
{
  double dx = one.x - two.x;
  double dy = one.y - two.y;
  double dist = sqrt(dx * dx + dy * dy);
  return dist < 0.2;
}

namespace explore
{
Explore::Explore()
  : Node("explore_node")
  , tf_buffer_(this->get_clock())
  , tf_listener_(tf_buffer_)
  , costmap_client_(*this, &tf_buffer_)
  , prev_distance_(0)
  , last_markers_count_(0)
  , task_succeed_server_(create_service<turtlebot3_msgs::srv::RingGoal>("task_succeed", std::bind(&Explore::taskSucceedCallback, this, std::placeholders::_1, std::placeholders::_2)))
  , task_fail_server_(create_service<turtlebot3_msgs::srv::RingGoal>("task_fail", std::bind(&Explore::taskFailCallback, this, std::placeholders::_1, std::placeholders::_2)))
{
  double timeout;
  double min_frontier_size;
  this->declare_parameter<float>("planner_frequency", 1.0);
  this->declare_parameter<float>("progress_timeout", 30.0);
  this->declare_parameter<bool>("visualize", true);
  this->declare_parameter<float>("potential_scale", 1e-3);
  this->declare_parameter<float>("orientation_scale", 0.0);
  this->declare_parameter<float>("gain_scale", 1.0);
  this->declare_parameter<float>("min_frontier_size", 0.5);
  this->declare_parameter<bool>("return_to_init", false);

  this->get_parameter("planner_frequency", planner_frequency_);
  this->get_parameter("progress_timeout", timeout);
  this->get_parameter("visualize", visualize_);
  this->get_parameter("potential_scale", potential_scale_);
  this->get_parameter("orientation_scale", orientation_scale_);
  this->get_parameter("gain_scale", gain_scale_);
  this->get_parameter("min_frontier_size", min_frontier_size);
  this->get_parameter("return_to_init", return_to_init_);
  this->get_parameter("robot_base_frame", robot_base_frame_);

  progress_timeout_ = timeout;
  // move_base_client_ =
  //     rclcpp_action::create_client<nav2_msgs::action::NavigateToPose>(
  //         this, ACTION_NAME);
  goal_pose_pub_ = this->create_publisher<geometry_msgs::msg::Pose>("goal_pose", 10);

  search_ = frontier_exploration::FrontierSearch(costmap_client_.getCostmap(),
                                                 potential_scale_, gain_scale_,
                                                 min_frontier_size);

  if (visualize_) {
    marker_array_publisher_ =
        this->create_publisher<visualization_msgs::msg::MarkerArray>("explore/"
                                                                     "frontier"
                                                                     "s",
                                                                     10);
  }

  // Subscription to resume or stop exploration
  resume_subscription_ = this->create_subscription<std_msgs::msg::Bool>(
      "explore/resume", 10,
      std::bind(&Explore::resumeCallback, this, std::placeholders::_1));
  
  auto callback = [this](const geometry_msgs::msg::Twist::SharedPtr msg) -> void {
      current_linear_vel = msg->linear.x;
      current_anguler_vel = msg->angular.z;

      // std::cout << "cmd_vel : " << current_linear_vel << ", " << current_anguler_vel << std::endl;
    };
  cmd_vel_subscriber_ = create_subscription<geometry_msgs::msg::Twist>("cmd_vel", 10, callback);
  

/*
  RCLCPP_INFO(logger_, "Waiting to connect to move_base nav2 server");
  move_base_client_->wait_for_action_server();
  RCLCPP_INFO(logger_, "Connected to move_base nav2 server");
*/

  if (return_to_init_) {
    RCLCPP_INFO(logger_, "Getting initial pose of the robot");
    geometry_msgs::msg::TransformStamped transformStamped;
    std::string map_frame = costmap_client_.getGlobalFrameID();
    try {
      transformStamped = tf_buffer_.lookupTransform(
          map_frame, robot_base_frame_, tf2::TimePointZero);
      initial_pose_.position.x = transformStamped.transform.translation.x;
      initial_pose_.position.y = transformStamped.transform.translation.y;
      initial_pose_.orientation = transformStamped.transform.rotation;
    } catch (tf2::TransformException& ex) {
      RCLCPP_ERROR(logger_, "Couldn't find transform from %s to %s: %s",
                   map_frame.c_str(), robot_base_frame_.c_str(), ex.what());
      return_to_init_ = false;
    }
  }

  exploring_timer_ = this->create_wall_timer(
      std::chrono::milliseconds((uint16_t)(1000.0 / planner_frequency_)),
      [this]() { makePlan(); });
  // Start exploration right away
  exploring_timer_->execute_callback();
}

Explore::~Explore()
{
  stop();
}

void Explore::resumeCallback(const std_msgs::msg::Bool::SharedPtr msg)
{
  if (msg->data) {
    resume();
  } else {
    stop();
  }
}

void Explore::visualizeFrontiers(
    const std::vector<frontier_exploration::Frontier>& frontiers)
{
  std_msgs::msg::ColorRGBA blue;
  blue.r = 0;
  blue.g = 0;
  blue.b = 1.0;
  blue.a = 1.0;
  std_msgs::msg::ColorRGBA red;
  red.r = 1.0;
  red.g = 0;
  red.b = 0;
  red.a = 1.0;
  std_msgs::msg::ColorRGBA green;
  green.r = 0;
  green.g = 1.0;
  green.b = 0;
  green.a = 1.0;

  RCLCPP_DEBUG(logger_, "visualising %lu frontiers", frontiers.size());
  visualization_msgs::msg::MarkerArray markers_msg;
  std::vector<visualization_msgs::msg::Marker>& markers = markers_msg.markers;
  visualization_msgs::msg::Marker m;

  m.header.frame_id = costmap_client_.getGlobalFrameID();
  m.header.stamp = this->now();
  m.ns = "frontiers";
  m.scale.x = 1.0;
  m.scale.y = 1.0;
  m.scale.z = 1.0;
  m.color.r = 0;
  m.color.g = 0;
  m.color.b = 255;
  m.color.a = 255;
  // lives forever
#ifdef ELOQUENT
  m.lifetime = rclcpp::Duration(0);  // deprecated in galactic warning
#elif DASHING
  m.lifetime = rclcpp::Duration(0);  // deprecated in galactic warning
#else
  m.lifetime = rclcpp::Duration::from_seconds(0);  // foxy onwards
#endif
  // m.lifetime = rclcpp::Duration::from_nanoseconds(0); // suggested in
  // galactic
  m.frame_locked = true;

  // weighted frontiers are always sorted
  double min_cost = frontiers.empty() ? 0. : frontiers.front().cost;

  m.action = visualization_msgs::msg::Marker::ADD;
  size_t id = 0;
  for (auto& frontier : frontiers) {
    m.type = visualization_msgs::msg::Marker::POINTS;
    m.id = int(id);
    // m.pose.position = {}; // compile warning
    m.scale.x = 0.1;
    m.scale.y = 0.1;
    m.scale.z = 0.1;
    m.points = frontier.points;
    if (goalOnBlacklist(frontier.centroid)) {
      m.color = red;
    } else {
      m.color = blue;
    }
    markers.push_back(m);
    ++id;
    m.type = visualization_msgs::msg::Marker::SPHERE;
    m.id = int(id);
    m.pose.position = frontier.initial;
    // scale frontier according to its cost (costier frontiers will be smaller)
    double scale = std::min(std::abs(min_cost * 0.4 / frontier.cost), 0.5);
    m.scale.x = scale;
    m.scale.y = scale;
    m.scale.z = scale;
    m.points = {};
    m.color = green;
    markers.push_back(m);
    ++id;
  }
  size_t current_markers_count = markers.size();

  // delete previous markers, which are now unused
  m.action = visualization_msgs::msg::Marker::DELETE;
  for (; id < last_markers_count_; ++id) {
    m.id = int(id);
    markers.push_back(m);
  }

  last_markers_count_ = current_markers_count;
  marker_array_publisher_->publish(markers_msg);
}

void Explore::makePlan()
{
  geometry_msgs::msg::Pose goal_pose;
  // RCLCPP_WARN(logger_, "Debug Point 0");
  // find frontiers
  auto pose = costmap_client_.getRobotPose();
  // get frontiers sorted according to cost
  auto start_time_search = std::chrono::high_resolution_clock::now();

  auto frontiers = search_.searchFrom(pose.position);

  auto end_time_search = std::chrono::high_resolution_clock::now();
  auto duration_search = std::chrono::duration_cast<std::chrono::microseconds>(end_time_search - start_time_search);
  
  std::cout << "Frontier Search Runtime = " << duration_search.count() << " micro second." << std::endl;
  
  RCLCPP_DEBUG(logger_, "found %lu frontiers", frontiers.size());
  RCLCPP_DEBUG(logger_, "Number of blacklist = %lu", frontier_blacklist_.size());
  // for (size_t i = 0; i < frontiers.size(); ++i) {
  //   RCLCPP_DEBUG(logger_, "frontier %zd cost: %f", i, frontiers[i].cost);
  // }
  
  if (frontiers.empty()) {
    RCLCPP_WARN(logger_, "No frontiers found, stopping.");
    stop(true);
    return;
  }
  // RCLCPP_WARN(logger_, "Debug Point 1");
  // publish frontiers as visualization markers
  if (visualize_) {
    visualizeFrontiers(frontiers);
  }
  // RCLCPP_WARN(logger_, "Debug Point 2");
  // find non blacklisted frontier
  auto frontier =
      std::find_if_not(frontiers.begin(), frontiers.end(),
                       [this](const frontier_exploration::Frontier& f) {
                         return goalOnBlacklist(f.centroid);
                       });
  if (frontier == frontiers.end()) {
    RCLCPP_WARN(logger_, "All frontiers traversed/tried out, stopping.");
    stop(true);
    return;
  }
  geometry_msgs::msg::Point target_position = frontier->centroid;
  // RCLCPP_WARN(logger_, "Debug Point 3");
  // time out if we are not making any progress
  bool same_goal = same_point(prev_goal_, target_position);
  prev_goal_ = target_position;
  if (!same_goal || prev_distance_ > frontier->min_distance) {
    // we have different goal or we made some progress
    last_progress_ = this->now();
    prev_distance_ = frontier->min_distance;
  }
  // RCLCPP_WARN(logger_, "Debug Point 4");
  // somehow goal is not published so republish target point if robot is not moving in sort of time

  if ((this->now() - last_progress_ >
      tf2::durationFromSec(5.0)) && !resuming_) {
        goal_pose.position.x = target_position.x;
        goal_pose.position.y = target_position.y;
        goal_pose.position.z = target_position.z;
        goal_pose.orientation.x = 0.0;
        goal_pose.orientation.y = 0.0;
        goal_pose.orientation.z = 0.0;
        goal_pose.orientation.w = 1.0;

        goal_pose_pub_->publish(goal_pose);
        RCLCPP_INFO(logger_, "Republishing frontier goal : %f, %f\n", goal_pose.position.x, goal_pose.position.y);
      }

  // black list if we've made no progress for a long time
  if ((this->now() - last_progress_ >
      tf2::durationFromSec(progress_timeout_)) && !resuming_) {
    frontier_blacklist_.push_back(target_position);
    RCLCPP_WARN(logger_, "Timeout!! Adding current goal to black list");
    makePlan();
    return;
  }

  // RCLCPP_WARN(logger_, "Debug Point 5");

  // ensure only first call of makePlan was set resuming to true
  if (resuming_) {
    resuming_ = false;
  }

  // we don't need to do anything if we still pursuing the same goal
  if (same_goal) {
    RCLCPP_DEBUG(logger_, "Same goal detected.");
    // TODO //
    // add black list if cmd_vel is all 0 //
    // if(current_linear_vel == 0 && current_anguler_vel == 0){
    //   frontier_blacklist_.push_back(target_position);
    //   RCLCPP_WARN(logger_, "Bug spot!! add current posision to black list");
    // }

    return;
  }

  // RCLCPP_WARN(logger_, "Debug Point 6");



  goal_pose.position.x = target_position.x;
  goal_pose.position.y = target_position.y;
  goal_pose.position.z = target_position.z;
  goal_pose.orientation.x = 0.0;
  goal_pose.orientation.y = 0.0;
  goal_pose.orientation.z = 0.0;
  goal_pose.orientation.w = 1.0;

  // Publish the goal pose
  goal_pose_pub_->publish(goal_pose);
  RCLCPP_INFO(logger_, "Frontier goal : %f, %f\n", goal_pose.position.x, goal_pose.position.y);

  // return;
}

void Explore::returnToInitialPose()
{
  RCLCPP_INFO(logger_, "Returning to initial pose.");
    geometry_msgs::msg::Pose goal_pose;
    goal_pose.position = initial_pose_.position;
    goal_pose.orientation = initial_pose_.orientation;
    
    // Publish the goal pose
    goal_pose_pub_->publish(goal_pose);

  // auto send_goal_options =
  //     rclcpp_action::Client<nav2_msgs::action::NavigateToPose>::SendGoalOptions();
  // move_base_client_->async_send_goal(goal, send_goal_options);
}

bool Explore::goalOnBlacklist(const geometry_msgs::msg::Point& goal)
{
  constexpr static size_t tolerace = 5;
  nav2_costmap_2d::Costmap2D* costmap2d = costmap_client_.getCostmap();

  // check if a goal is on the blacklist for goals that we're pursuing
  for (auto& frontier_goal : frontier_blacklist_) {
    double x_diff = fabs(goal.x - frontier_goal.x);
    double y_diff = fabs(goal.y - frontier_goal.y);

    if (x_diff < tolerace * costmap2d->getResolution() &&
        y_diff < tolerace * costmap2d->getResolution())
      return true;
  }
  return false;
}

void Explore::reachedGoal(const NavigationGoalHandle::WrappedResult& result,
                          const geometry_msgs::msg::Point& frontier_goal)
{
  switch (result.code) {
    case rclcpp_action::ResultCode::SUCCEEDED:
      RCLCPP_DEBUG(logger_, "Goal was successful");
      break;
    case rclcpp_action::ResultCode::ABORTED:
      RCLCPP_DEBUG(logger_, "Goal was aborted");
      frontier_blacklist_.push_back(frontier_goal);
      RCLCPP_DEBUG(logger_, "Adding current goal to black list");
      // If it was aborted probably because we've found another frontier goal,
      // so just return and don't make plan again
      return;
    case rclcpp_action::ResultCode::CANCELED:
      RCLCPP_DEBUG(logger_, "Goal was canceled");
      // If goal canceled might be because exploration stopped from topic. Don't make new plan.
      return;
    default:
      RCLCPP_WARN(logger_, "Unknown result code from move base nav2");
      break;
  }
  // find new goal immediately regardless of planning frequency.
  // execute via timer to prevent dead lock in move_base_client (this is
  // callback for sendGoal, which is called in makePlan). the timer must live
  // until callback is executed.
  // oneshot_ = relative_nh_.createTimer(
  //     ros::Duration(0, 0), [this](const ros::TimerEvent&) { makePlan(); },
  //     true);

  // Because of the 1-thread-executor nature of ros2 I think timer is not
  // needed.
  makePlan();
}

void Explore::start()
{
  RCLCPP_INFO(logger_, "Exploration started.");
}

void Explore::stop(bool finished_exploring)
{
  RCLCPP_INFO(logger_, "Exploration stopped.");
  // move_base_client_->async_cancel_all_goals();
  exploring_timer_->cancel();

  if (return_to_init_ && finished_exploring) {
    returnToInitialPose();
  }
}

void Explore::resume()
{
  resuming_ = true;
  RCLCPP_INFO(logger_, "Exploration resuming.");
  // Reactivate the timer
  exploring_timer_->reset();
  // Resume immediately
  exploring_timer_->execute_callback();
}


void Explore::taskSucceedCallback(
    [[maybe_unused]] const std::shared_ptr<turtlebot3_msgs::srv::RingGoal::Request> request,
    [[maybe_unused]] std::shared_ptr<turtlebot3_msgs::srv::RingGoal::Response> response)
{
  RCLCPP_INFO(logger_, "Goal was success");
  // geometry_msgs::msg::Point frontier_goal_;
  // frontier_goal_.x = request->robot_pose_x;
  // frontier_goal_.y = request->robot_pose_y;
  // frontier_goal_.z = 0;
  // frontier_blacklist_.push_back(frontier_goal_);
  makePlan();
}

void Explore::taskFailCallback(
    [[maybe_unused]] const std::shared_ptr<turtlebot3_msgs::srv::RingGoal::Request> request,
    [[maybe_unused]] std::shared_ptr<turtlebot3_msgs::srv::RingGoal::Response> response)
{
  geometry_msgs::msg::Point frontier_goal_;
  frontier_goal_.x = request->robot_pose_x;
  frontier_goal_.y = request->robot_pose_y;
  frontier_goal_.z = 0;
  RCLCPP_INFO(logger_, "Goal was aborted");
  frontier_blacklist_.push_back(frontier_goal_);
  RCLCPP_INFO(logger_, "Adding current goal to black list");
  sleep(1);
  makePlan();
}

}  // namespace explore

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  // ROS1 code
  /*
  if (ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME,
                                     ros::console::levels::Debug)) {
    ros::console::notifyLoggerLevelsChanged();
  } */
  rclcpp::spin(
      std::make_shared<explore::Explore>());  // std::move(std::make_unique)?
  rclcpp::shutdown();
  return 0;
}