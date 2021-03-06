#include "systems/controllers/cp_traj_gen.h"

#include <math.h>
#include <algorithm>  // std::min
#include <string>

#include "systems/controllers/control_utils.h"

using std::cout;
using std::endl;
using std::string;

using Eigen::MatrixXd;
using Eigen::Vector2d;
using Eigen::Vector3d;
using Eigen::VectorXd;

using drake::systems::BasicVector;
using drake::systems::Context;
using drake::systems::DiscreteUpdateEvent;
using drake::systems::DiscreteValues;
using drake::systems::EventStatus;

using drake::trajectories::ExponentialPlusPiecewisePolynomial;
using drake::trajectories::PiecewisePolynomial;

namespace dairlib {
namespace systems {

CPTrajGenerator::CPTrajGenerator(
    const RigidBodyTree<double>& tree,
    std::vector<int> left_right_support_fsm_states,
    std::vector<double> left_right_support_state_durations,
    std::vector<int> left_right_foot_body_indices,
    std::vector<Vector3d> left_right_foot_pts_on_bodies, int pelvis_idx,
    double mid_foot_height, double desired_final_foot_height,
    double desired_final_vertical_foot_velocity, double max_CoM_to_CP_dist,
    bool add_extra_control, bool is_feet_collision_avoid,
    bool is_using_predicted_com, double cp_offset, double center_line_offset)
    : tree_(tree),
      left_right_support_fsm_states_(left_right_support_fsm_states),
      left_right_support_state_durations_(left_right_support_state_durations),
      left_right_foot_body_indices_(left_right_foot_body_indices),
      left_right_foot_pts_on_bodies_(left_right_foot_pts_on_bodies),
      pelvis_idx_(pelvis_idx),
      mid_foot_height_(mid_foot_height),
      desired_final_foot_height_(desired_final_foot_height),
      desired_final_vertical_foot_velocity_(
          desired_final_vertical_foot_velocity),
      max_CoM_to_CP_dist_(max_CoM_to_CP_dist),
      add_extra_control_(add_extra_control),
      is_feet_collision_avoid_(is_feet_collision_avoid),
      is_using_predicted_com_(is_using_predicted_com),
      cp_offset_(cp_offset),
      center_line_offset_(center_line_offset) {
  this->set_name("cp_traj");

  // Input/Output Setup
  state_port_ = this
                    ->DeclareVectorInputPort(OutputVector<double>(
                        tree.get_num_positions(), tree.get_num_velocities(),
                        tree.get_num_actuators()))
                    .get_index();
  fsm_port_ = this->DeclareVectorInputPort(BasicVector<double>(1)).get_index();

  PiecewisePolynomial<double> pp(VectorXd::Zero(0));
  if (is_using_predicted_com) {
    com_port_ =
        this->DeclareAbstractInputPort(
                "CoM_traj",
                drake::Value<drake::trajectories::Trajectory<double>>(pp))
            .get_index();
  }
  if (add_extra_control) {
    fp_port_ = this->DeclareVectorInputPort(BasicVector<double>(2)).get_index();
  }
  // Provide an instance to allocate the memory first (for the output)
  drake::trajectories::Trajectory<double>& traj_instance = pp;
  this->DeclareAbstractOutputPort("cp_traj", traj_instance,
                                  &CPTrajGenerator::CalcTrajs);

  // State variables inside this controller block
  DeclarePerStepDiscreteUpdateEvent(&CPTrajGenerator::DiscreteVariableUpdate);
  // The swing foot position in the beginning of the swing phase
  prev_td_swing_foot_idx_ = this->DeclareDiscreteState(3);
  // The time of the last touch down
  prev_td_time_idx_ = this->DeclareDiscreteState(1);
  // The last state of FSM
  prev_fsm_state_idx_ = this->DeclareDiscreteState(-0.1 * VectorXd::Ones(1));

  // Check if the model is floating based
  is_quaternion_ = multibody::IsFloatingBase(tree);
}

EventStatus CPTrajGenerator::DiscreteVariableUpdate(
    const Context<double>& context,
    DiscreteValues<double>* discrete_state) const {
  // Read in finite state machine
  const BasicVector<double>* fsm_output =
      (BasicVector<double>*)this->EvalVectorInput(context, fsm_port_);
  VectorXd fsm_state = fsm_output->get_value();

  auto prev_fsm_state = discrete_state->get_mutable_vector(prev_fsm_state_idx_)
                            .get_mutable_value();

  if (fsm_state(0) != prev_fsm_state(0)) {  // if at touchdown
    prev_fsm_state(0) = fsm_state(0);

    auto swing_foot_pos_td =
        discrete_state->get_mutable_vector(prev_td_swing_foot_idx_)
            .get_mutable_value();
    auto prev_td_time = discrete_state->get_mutable_vector(prev_td_time_idx_)
                            .get_mutable_value();

    // Read in current state
    const OutputVector<double>* robot_output =
        (OutputVector<double>*)this->EvalVectorInput(context, state_port_);

    // Get time
    double timestamp = robot_output->get_timestamp();
    double current_time = static_cast<double>(timestamp);
    prev_td_time(0) = current_time;

    // Kinematics cache and indices
    KinematicsCache<double> cache = tree_.CreateKinematicsCache();
    VectorXd q = robot_output->GetPositions();
    // Modify the quaternion in the begining when the state is not received from
    // the robot yet (cannot have 0-norm quaternion when using doKinematics)
    if (is_quaternion_) {
      multibody::SetZeroQuaternionToIdentity(&q);
    }
    cache.initialize(q);
    tree_.doKinematics(cache);
    int swing_foot_idx = (fsm_state(0) == left_right_support_fsm_states_[1])
                             ? left_right_foot_body_indices_[0]
                             : left_right_foot_body_indices_[1];
    Vector3d pt_on_swing_foot =
        (fsm_state(0) == left_right_support_fsm_states_[1])
            ? left_right_foot_pts_on_bodies_[0]
            : left_right_foot_pts_on_bodies_[1];

    // Swing foot position (Forward Kinematics) at touchdown
    swing_foot_pos_td =
        tree_.transformPoints(cache, pt_on_swing_foot, swing_foot_idx, 0);
  }

  return EventStatus::Succeeded();
}

void CPTrajGenerator::calcCpAndStanceFootHeight(
    const Context<double>& context, const OutputVector<double>* robot_output,
    const double end_time_of_this_interval, Vector2d* final_CP,
    VectorXd* stance_foot_height) const {
  // Read in finite state machine
  const BasicVector<double>* fsm_output =
      (BasicVector<double>*)this->EvalVectorInput(context, fsm_port_);
  VectorXd fsm_state = fsm_output->get_value();

  // Get stance foot position and index
  KinematicsCache<double> cache = tree_.CreateKinematicsCache();
  VectorXd q = robot_output->GetPositions();
  // Modify the quaternion in the begining when the state is not received from
  // the robot yet
  if (is_quaternion_) {
    multibody::SetZeroQuaternionToIdentity(&q);
  }
  cache.initialize(q);
  tree_.doKinematics(cache);

  int stance_foot_idx;
  Vector3d pt_on_stance_foot;
  if (fsm_state(0) == left_right_support_fsm_states_[1]) {
    stance_foot_idx = left_right_foot_body_indices_[1];
    pt_on_stance_foot = left_right_foot_pts_on_bodies_[1];
  } else {
    stance_foot_idx = left_right_foot_body_indices_[0];
    pt_on_stance_foot = left_right_foot_pts_on_bodies_[0];
  }
  Vector3d stance_foot_pos =
      tree_.transformPoints(cache, pt_on_stance_foot, stance_foot_idx, 0);

  // Get CoM or predicted CoM
  Vector3d CoM;
  Vector3d dCoM;
  if (is_using_predicted_com_) {
    // CoM and dCoM at the end of the step (predicted)
    const drake::AbstractValue* com_traj_output =
        this->EvalAbstractInput(context, com_port_);
    DRAKE_ASSERT(com_traj_output != nullptr);
    const auto& com_traj =
        com_traj_output->get_value<drake::trajectories::Trajectory<double>>();
    CoM = com_traj.value(end_time_of_this_interval);
    dCoM = com_traj.MakeDerivative(1)->value(end_time_of_this_interval);
  } else {
    // Get the current center of mass position and velocity
    MatrixXd J_com = tree_.centerOfMassJacobian(cache);
    VectorXd v = robot_output->GetVelocities();
    CoM = tree_.centerOfMass(cache);
    dCoM = J_com * v;
  }

  double pred_omega = sqrt(9.81 / CoM(2));

  Vector2d CP;
  CP << (CoM(0) + dCoM(0) / pred_omega), (CoM(1) + dCoM(1) / pred_omega);

  // Walking position control
  if (add_extra_control_) {
    // Read in foot placement
    const BasicVector<double>* fp_output =
        (BasicVector<double>*)this->EvalVectorInput(context, fp_port_);
    CP += fp_output->get_value();
  }

  if (is_feet_collision_avoid_) {
    // Get approximated heading angle of pelvis
    Vector3d pelvis_heading_vec =
        tree_.CalcBodyPoseInWorldFrame(cache, tree_.get_body(pelvis_idx_))
            .linear()
            .col(0);
    double approx_pelvis_yaw =
        atan2(pelvis_heading_vec(1), pelvis_heading_vec(0));

    // Shift CP a little away from CoM line and toward the swing foot, so that
    // the foot placement position at steady state is right below the hip joint
    Vector2d shift_foothold_dir;
    if (fsm_state(0) == left_right_support_fsm_states_[1]) {
      shift_foothold_dir << cos(approx_pelvis_yaw + M_PI * 1 / 2),
          sin(approx_pelvis_yaw + M_PI * 1 / 2);
    } else {
      shift_foothold_dir << cos(approx_pelvis_yaw + M_PI * 3 / 2),
          sin(approx_pelvis_yaw + M_PI * 3 / 2);
    }
    CP = CP + shift_foothold_dir * cp_offset_;

    CP = ImposeHalfplaneGuard(
        CP, (fsm_state(0) == left_right_support_fsm_states_[0]),
        approx_pelvis_yaw, CoM.head(2), stance_foot_pos.head(2),
        center_line_offset_);
  }

  // Cap by the step length
  CP = ImposeStepLengthGuard(CP, CoM.head(2), max_CoM_to_CP_dist_);

  // Assignment
  (*stance_foot_height)(0) = stance_foot_pos(2);
  *final_CP = CP;
}

PiecewisePolynomial<double> CPTrajGenerator::createSplineForSwingFoot(
    const double start_time_of_this_interval,
    const double end_time_of_this_interval, const double stance_duration,
    const Vector3d& init_swing_foot_pos, const Vector2d& CP,
    const VectorXd& stance_foot_height) const {
  // Two segment of cubic polynomial with velocity constraints
  std::vector<double> T_waypoint = {
      start_time_of_this_interval,
      (start_time_of_this_interval + end_time_of_this_interval) / 2,
      end_time_of_this_interval};

  std::vector<MatrixXd> Y(T_waypoint.size(), MatrixXd::Zero(3, 1));
  // x
  Y[0](0, 0) = init_swing_foot_pos(0);
  Y[1](0, 0) = (init_swing_foot_pos(0) + CP(0)) / 2;
  Y[2](0, 0) = CP(0);
  // y
  Y[0](1, 0) = init_swing_foot_pos(1);
  Y[1](1, 0) = (init_swing_foot_pos(1) + CP(1)) / 2;
  Y[2](1, 0) = CP(1);
  // z
  /// We added stance_foot_height because we want the desired trajectory to be
  /// relative to the stance foot in case the floating base state estimation
  /// drifts.
  Y[0](2, 0) = init_swing_foot_pos(2);
  Y[1](2, 0) = mid_foot_height_ + stance_foot_height(0);
  Y[2](2, 0) = desired_final_foot_height_ + stance_foot_height(0);

  std::vector<MatrixXd> Y_dot(T_waypoint.size(), MatrixXd::Zero(3, 1));
  // x
  Y_dot[0](0, 0) = 0;
  Y_dot[1](0, 0) = (CP(0) - init_swing_foot_pos(0)) / stance_duration;
  Y_dot[2](0, 0) = 0;
  // y
  Y_dot[0](1, 0) = 0;
  Y_dot[1](1, 0) = (CP(1) - init_swing_foot_pos(1)) / stance_duration;
  Y_dot[2](1, 0) = 0;
  // z
  Y_dot[0](2, 0) = 0;
  Y_dot[1](2, 0) = 0;
  Y_dot[2](2, 0) = desired_final_vertical_foot_velocity_;
  PiecewisePolynomial<double> swing_foot_spline =
      PiecewisePolynomial<double>::CubicHermite(T_waypoint, Y, Y_dot);

  return swing_foot_spline;
}

void CPTrajGenerator::CalcTrajs(
    const Context<double>& context,
    drake::trajectories::Trajectory<double>* traj) const {
  // Cast traj for polymorphism
  PiecewisePolynomial<double>* pp_traj =
      (PiecewisePolynomial<double>*)dynamic_cast<PiecewisePolynomial<double>*>(
          traj);

  // Get discrete states
  const auto swing_foot_pos_td =
      context.get_discrete_state(prev_td_swing_foot_idx_).get_value();
  const auto prev_td_time =
      context.get_discrete_state(prev_td_time_idx_).get_value();

  // Read in finite state machine
  const BasicVector<double>* fsm_output =
      (BasicVector<double>*)this->EvalVectorInput(context, fsm_port_);
  VectorXd fsm_state = fsm_output->get_value();

  // Find fsm_state in left_right_support_fsm_states
  auto it = find(left_right_support_fsm_states_.begin(),
                 left_right_support_fsm_states_.end(), int(fsm_state(0)));
  int index = std::distance(left_right_support_fsm_states_.begin(), it);

  // swing phase if current state is in left_right_support_fsm_states_
  bool is_swing_phase = !(it == left_right_support_fsm_states_.end());

  // Generate trajectory based on CP if it's currently in swing phase.
  // Otherwise, generate a constant trajectory in case the user still tracks
  // the trajectory accidentally.
  if (is_swing_phase) {
    // Read in current robot state
    const OutputVector<double>* robot_output =
        (OutputVector<double>*)this->EvalVectorInput(context, state_port_);

    // Get current time
    double timestamp = robot_output->get_timestamp();
    auto current_time = static_cast<double>(timestamp);

    // Get the start time and the end time of the current stance phase
    double start_time_of_this_interval = prev_td_time(0);
    double end_time_of_this_interval =
        prev_td_time(0) + left_right_support_state_durations_[index];

    // Ensure current_time < end_time_of_this_interval to avoid error in
    // creating trajectory.
    if ((end_time_of_this_interval <= current_time + 0.001)) {
      end_time_of_this_interval = current_time + 0.002;
    }

    // Get Capture Point
    VectorXd stance_foot_height = VectorXd::Zero(1);
    Vector2d CP(0, 0);
    calcCpAndStanceFootHeight(context, robot_output, end_time_of_this_interval,
                              &CP, &stance_foot_height);

    // Swing foot position at touchdown
    Vector3d init_swing_foot_pos = swing_foot_pos_td;

    // Assign traj
    *pp_traj = createSplineForSwingFoot(
        start_time_of_this_interval, end_time_of_this_interval,
        left_right_support_state_durations_[index], init_swing_foot_pos, CP,
        stance_foot_height);

  } else {
    // Assign a constant traj
    *pp_traj = PiecewisePolynomial<double>(swing_foot_pos_td);
  }
}
}  // namespace systems
}  // namespace dairlib
