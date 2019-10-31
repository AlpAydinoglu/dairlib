#include "examples/jumping/flight_foot_traj.h"

using std::cout;
using std::endl;
using std::string;

using Eigen::Vector2d;
using Eigen::Vector3d;
using Eigen::VectorXd;
using Eigen::MatrixXd;

using drake::systems::Context;
using drake::systems::DiscreteValues;
using drake::systems::DiscreteUpdateEvent;
using drake::systems::EventStatus;
using drake::systems::BasicVector;
using dairlib::systems::OutputVector;
using drake::trajectories::Trajectory;
using drake::trajectories::PiecewisePolynomial;
using drake::trajectories::ExponentialPlusPiecewisePolynomial;

namespace dairlib {
namespace examples {
namespace jumping {
namespace osc {

FlightFootTraj::FlightFootTraj(const RigidBodyTree<double>& tree,
                               int hip_idx,
                               int left_foot_idx,
                               int right_foot_idx,
                               bool isLeftFoot,
                               double height,
                               double foot_offset) :
    tree_(tree),
    hip_idx_(hip_idx),
    left_foot_idx_(left_foot_idx),
    right_foot_idx_(right_foot_idx),
    isLeftFoot_(isLeftFoot),
    height_(height) {
  PiecewisePolynomial<double> empty_pp_traj(VectorXd(0));
  Trajectory<double>& traj_inst = empty_pp_traj;

  if (isLeftFoot) {
    this->set_name("l_foot_traj");
    this->DeclareAbstractOutputPort("l_foot_traj", traj_inst,
                                    &FlightFootTraj::CalcTraj);
    foot_offset_ = -foot_offset;
  } else {
    this->set_name("r_foot_traj");
    this->DeclareAbstractOutputPort("r_foot_traj", traj_inst,
                                    &FlightFootTraj::CalcTraj);
    foot_offset_ = foot_offset;
  }

  // Input/Output Setup
  state_port_ = this->DeclareVectorInputPort(OutputVector<double>(
      tree.get_num_positions(),
      tree.get_num_velocities(),
      tree.get_num_actuators())).get_index();
  fsm_port_ = this->DeclareVectorInputPort(
      BasicVector<double>(1)).get_index();

  // DeclarePerStepDiscreteUpdateEvent(&FlightFootTraj::DiscreteVariableUpdate);
}

/*
  Move the feet relative to the COM
  The trajectory of the COM cannot be altered, so must solve for
  foot positions as a function of COM.
*/
PiecewisePolynomial<double> FlightFootTraj::generateFlightTraj(
    const drake::systems::Context<double>& context,
    VectorXd* q, VectorXd* v) const {
  // Kinematics cache and indices
  KinematicsCache<double> cache = tree_.CreateKinematicsCache();
  // Modify the quaternion in the begining when the state is not received from
  // the robot yet
  // Always remember to check 0-norm quaternion when using doKinematics
  multibody::SetZeroQuaternionToIdentity(q);
  cache.initialize(*q);
  tree_.doKinematics(cache);

  // Vector3d pt_on_foot = Eigen::VectorXd::Zero(3);
  // find a function that calculates the center of mass for a rigidbodytree

  Vector3d center_of_mass = tree_.centerOfMass(cache);
//  Vector3d com_velocity = tree_.centerOfMassJacobianDotTimesV(cache);
//  VectorXd
  Vector3d desired_foot_pos(center_of_mass(0) - foot_offset_,
                            center_of_mass(1),
                            center_of_mass(2) - height_);

  // 0);

  return PiecewisePolynomial<double>(desired_foot_pos);
}

void FlightFootTraj::CalcTraj(const drake::systems::Context<double>&
context,
                              drake::trajectories::Trajectory<double>* traj) const {

  // Read in current state
  const OutputVector<double>* robot_output = (OutputVector<double>*)
      this->EvalVectorInput(context, state_port_);
  VectorXd q = robot_output->GetPositions();
  VectorXd v = robot_output->GetVelocities();

  // Read in finite state machine
  const BasicVector<double>* fsm_output = (BasicVector<double>*)
      this->EvalVectorInput(context, fsm_port_);
  VectorXd fsm_state = fsm_output->get_value();

  PiecewisePolynomial<double>* casted_traj = (PiecewisePolynomial<double>*)
      dynamic_cast<PiecewisePolynomial<double>*> (traj);
  switch ((int) fsm_state(0)) {
    case (2): //FLIGHT
      *casted_traj = generateFlightTraj(context, &q, &v);
      break;
    default:break;
  }
}

}  // namespace osc
}  // namespace jumping
}  // namespace examples
}  // namespace dairlib


