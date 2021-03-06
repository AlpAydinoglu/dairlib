#include <memory>
#include <chrono>

#include <gflags/gflags.h>

#include "drake/solvers/snopt_solver.h"
#include "drake/systems/analysis/simulator.h"
#include "drake/systems/framework/diagram.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/systems/primitives/trajectory_source.h"
#include "drake/lcm/drake_lcm.h"
#include "drake/solvers/mathematical_program.h"
#include "drake/solvers/constraint.h"
#include "drake/multibody/parsing/parser.h"
#include "drake/systems/rendering/multibody_position_to_geometry_pose.h"
#include "drake/geometry/geometry_visualization.h"
#include "drake/solvers/solve.h"

#include "common/find_resource.h"
#include "systems/primitives/subvector_pass_through.h"
#include "solvers/optimization_utils.h"
#include "systems/trajectory_optimization/dircon_position_data.h"
#include "systems/trajectory_optimization/dircon_kinematic_data_set.h"
#include "systems/trajectory_optimization/hybrid_dircon.h"
#include "systems/trajectory_optimization/dircon_opt_constraints.h"
#include "multibody/multibody_utils.h"
#include "multibody/visualization_utils.h"


DEFINE_double(strideLength, 0.1, "The stride length.");
DEFINE_double(duration, 1, "The stride duration");
DEFINE_bool(autodiff, false, "Double or autodiff version");

using drake::multibody::MultibodyPlant;
using drake::geometry::SceneGraph;
using drake::multibody::Body;
using drake::multibody::Parser;
using drake::systems::rendering::MultibodyPositionToGeometryPose;
using Eigen::Vector3d;
using Eigen::VectorXd;
using Eigen::MatrixXd;
using Eigen::Matrix3Xd;
using drake::trajectories::PiecewisePolynomial;
using std::vector;
using std::shared_ptr;
using std::cout;
using std::endl;


/// Inputs: initial trajectory
/// Outputs: trajectory optimization problem
namespace dairlib {
namespace {
using systems::trajectory_optimization::HybridDircon;
using systems::trajectory_optimization::DirconDynamicConstraint;
using systems::trajectory_optimization::DirconKinematicConstraint;
using systems::trajectory_optimization::DirconOptions;
using systems::trajectory_optimization::DirconKinConstraintType;
using systems::SubvectorPassThrough;


template <typename T>
shared_ptr<HybridDircon<T>> runDircon(
    std::unique_ptr<MultibodyPlant<T>> plant_ptr,
    MultibodyPlant<double>* plant_double_ptr,
    std::unique_ptr<SceneGraph<double>> scene_graph_ptr,
    double stride_length,
    double duration,
    PiecewisePolynomial<double> init_x_traj,
    PiecewisePolynomial<double> init_u_traj,
    vector<PiecewisePolynomial<double>> init_l_traj,
    vector<PiecewisePolynomial<double>> init_lc_traj,
    vector<PiecewisePolynomial<double>> init_vc_traj) {

  drake::systems::DiagramBuilder<double> builder;
  MultibodyPlant<T>& plant = *plant_ptr;
  SceneGraph<double>& scene_graph =
      *builder.AddSystem(std::move(scene_graph_ptr));

  auto positions_map = multibody::makeNameToPositionsMap(plant);
  auto velocities_map = multibody::makeNameToVelocitiesMap(plant);

  for (auto const& element : positions_map)
    cout << element.first << " = " << element.second << endl;
  for (auto const& element : velocities_map)
    cout << element.first << " = " << element.second << endl;

  const Body<T>& left_lower_leg = plant.GetBodyByName("left_lower_leg");
  const Body<T>& right_lower_leg = plant.GetBodyByName("right_lower_leg");

  Vector3d pt;
  pt << 0, 0, -.5;
  bool isXZ = true;

  Vector3d normal;
  normal << 0, 0, 1;
  auto leftFootConstraint = DirconPositionData<T>(plant, left_lower_leg,
                                                       pt, isXZ, normal);
  auto rightFootConstraint = DirconPositionData<T>(plant, right_lower_leg,
                                                        pt, isXZ, normal);

  double mu = 1;
  leftFootConstraint.addFixedNormalFrictionConstraints(mu);
  rightFootConstraint.addFixedNormalFrictionConstraints(mu);

  std::vector<DirconKinematicData<T>*> leftConstraints;
  leftConstraints.push_back(&leftFootConstraint);
  auto leftDataSet = DirconKinematicDataSet<T>(plant, &leftConstraints);

  std::vector<DirconKinematicData<T>*> rightConstraints;
  rightConstraints.push_back(&rightFootConstraint);
  auto rightDataSet = DirconKinematicDataSet<T>(plant, &rightConstraints);

  auto leftOptions = DirconOptions(leftDataSet.countConstraints());
  leftOptions.setConstraintRelative(0, true);

  auto rightOptions = DirconOptions(rightDataSet.countConstraints());
  rightOptions.setConstraintRelative(0, true);

  std::vector<int> timesteps;
  timesteps.push_back(10);
  timesteps.push_back(10);
  std::vector<double> min_dt;
  min_dt.push_back(.01);
  min_dt.push_back(.01);
  std::vector<double> max_dt;
  max_dt.push_back(.3);
  max_dt.push_back(.3);

  std::vector<DirconKinematicDataSet<T>*> dataset_list;
  dataset_list.push_back(&leftDataSet);
  dataset_list.push_back(&rightDataSet);

  std::vector<DirconOptions> options_list;
  options_list.push_back(leftOptions);
  options_list.push_back(rightOptions);

  auto trajopt = std::make_shared<HybridDircon<T>>(plant,
      timesteps, min_dt, max_dt, dataset_list, options_list);

  trajopt->AddDurationBounds(duration, duration);

  trajopt->SetSolverOption(drake::solvers::SnoptSolver::id(),
                           "Print file", "../snopt.out");
  trajopt->SetSolverOption(drake::solvers::SnoptSolver::id(),
                           "Major iterations limit", 100);

  // trajopt->SetSolverOption(drake::solvers::SnoptSolver::id(),
  //    "Verify level","1");

  for (uint j = 0; j < timesteps.size(); j++) {
    trajopt->drake::systems::trajectory_optimization::MultipleShooting::
        SetInitialTrajectory(init_u_traj, init_x_traj);
    trajopt->SetInitialForceTrajectory(j, init_l_traj[j], init_lc_traj[j],
                                      init_vc_traj[j]);
  }

  // Periodicity constraints
// hip_pin = 3
// left_knee_pin = 4
// planar_roty = 2
// planar_x = 0
// planar_z = 1
// right_knee_pin = 5
//
// hip_pindot = 3
// left_knee_pindot = 4
// planar_rotydot = 2
// planar_xdot = 0
// planar_zdot = 1
// right_knee_pindot = 5

  auto x0 = trajopt->initial_state();
  auto xf = trajopt->final_state();
  trajopt->AddLinearConstraint(
      x0(positions_map["planar_z"]) == xf(positions_map["planar_z"]));
  trajopt->AddLinearConstraint(x0(positions_map["hip_pin"]) +
      x0(positions_map["planar_roty"]) == xf(positions_map["planar_roty"]));
  trajopt->AddLinearConstraint(x0(positions_map["left_knee_pin"]) ==
      xf(positions_map["right_knee_pin"]));
  trajopt->AddLinearConstraint(x0(positions_map["right_knee_pin"]) ==
      xf(positions_map["left_knee_pin"]));
  trajopt->AddLinearConstraint(x0(positions_map["hip_pin"]) ==
      -xf(positions_map["hip_pin"]));


  int nq = plant.num_positions();
  trajopt->AddLinearConstraint(x0(nq + velocities_map["planar_zdot"]) ==
                               xf(nq + velocities_map["planar_zdot"]));
  trajopt->AddLinearConstraint(x0(nq + velocities_map["hip_pindot"]) +
                               x0(nq + velocities_map["planar_rotydot"]) ==
                               xf(nq + velocities_map["planar_rotydot"]));
  trajopt->AddLinearConstraint(x0(nq + velocities_map["left_knee_pindot"]) ==
                               xf(nq + velocities_map["right_knee_pindot"]));
  trajopt->AddLinearConstraint(x0(nq + velocities_map["right_knee_pindot"]) ==
                               xf(nq + velocities_map["left_knee_pindot"]));
  trajopt->AddLinearConstraint(x0(nq + velocities_map["hip_pindot"]) ==
                               -xf(nq + velocities_map["hip_pindot"]));

  // // Knee joint limits
  auto x = trajopt->state();
  trajopt->AddConstraintToAllKnotPoints(x(positions_map["left_knee_pin"]) >= 0);
  trajopt->AddConstraintToAllKnotPoints(
      x(positions_map["right_knee_pin"]) >= 0);

  // stride length constraints
  trajopt->AddLinearConstraint(x0(positions_map["planar_x"]) == 0);
  trajopt->AddLinearConstraint(xf(positions_map["planar_x"]) == stride_length);

  const double R = 10;  // Cost on input effort
  auto u = trajopt->input();
  trajopt->AddRunningCost(u.transpose()*R*u);
  // const double Q = 1;
  // trajopt->AddRunningCost(x.transpose()*Q*x);

  std::vector<unsigned int> visualizer_poses;
  visualizer_poses.push_back(3);
  visualizer_poses.push_back(3);

  trajopt->CreateVisualizationCallback(
      dairlib::FindResourceOrThrow("examples/PlanarWalker/PlanarWalker.urdf"),
      visualizer_poses, "base");

  auto start = std::chrono::high_resolution_clock::now();
  const auto result = Solve(*trajopt, trajopt->initial_guess());
  auto finish = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = finish - start;
  std::cout << "Solve time:" << elapsed.count() <<std::endl;
  std::cout << "Cost:" << result.get_optimal_cost() <<std::endl;

  // systems::trajectory_optimization::checkConstraints(trajopt.get(), result);

  MatrixXd A;
  VectorXd y, lb, ub;
  VectorXd x_sol = result.get_x_val();
  solvers::LinearizeConstraints(*trajopt, x_sol, &y, &A, &lb, &ub);

//  MatrixXd y_and_bounds(y.size(),3);
//  y_and_bounds.col(0) = lb;
//  y_and_bounds.col(1) = y;
//  y_and_bounds.col(2) = ub;
//  cout << "*************y***************" << endl;
//  cout << y_and_bounds << endl;
//  cout << "*************A***************" << endl;
//  cout << A << endl;

  // visualizer
  const drake::trajectories::PiecewisePolynomial<double> pp_xtraj =
      trajopt->ReconstructStateTrajectory(result);
  multibody::connectTrajectoryVisualizer(plant_double_ptr,
      &builder, &scene_graph, pp_xtraj);
  auto diagram = builder.Build();

  while (true) {
    drake::systems::Simulator<double> simulator(*diagram);
    simulator.set_target_realtime_rate(.5);
    simulator.Initialize();
    simulator.AdvanceTo(pp_xtraj.end_time());
  }

  return trajopt;
}
}  // namespace
}  // namespace dairlib


int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  std::srand(time(0));  // Initialize random number generator.

  auto plant = std::make_unique<MultibodyPlant<double>>();
  auto scene_graph = std::make_unique<SceneGraph<double>>();
  Parser parser(plant.get(), scene_graph.get());
  std::string full_name =
      dairlib::FindResourceOrThrow("examples/PlanarWalker/PlanarWalker.urdf");

  parser.AddModelFromFile(full_name);

  plant->mutable_gravity_field().set_gravity_vector(
      -9.81 * Eigen::Vector3d::UnitZ());

  plant->WeldFrames(
      plant->world_frame(), plant->GetFrameByName("base"),
      drake::math::RigidTransform<double>());

  plant->Finalize();

  Eigen::VectorXd x0 = Eigen::VectorXd::Zero(plant->num_positions() +
                       plant->num_velocities());

  Eigen::VectorXd init_l_vec(2);
  init_l_vec << 0, 20*9.81;
  int nu = plant->num_actuators();
  int nx = plant->num_positions() + plant->num_velocities();
  int N = 10;

  std::vector<MatrixXd> init_x;
  std::vector<MatrixXd> init_u;
  std::vector<PiecewisePolynomial<double>> init_l_traj;
  std::vector<PiecewisePolynomial<double>> init_lc_traj;
  std::vector<PiecewisePolynomial<double>> init_vc_traj;

  // Initialize state trajectory
  std::vector<double> init_time;
  for (int i = 0; i < 2*N-1; i++) {
    init_time.push_back(i*.2);
    init_x.push_back(x0 + .1*VectorXd::Random(nx));
    init_u.push_back(VectorXd::Random(nu));
  }
  auto init_x_traj = PiecewisePolynomial<double>::ZeroOrderHold(init_time, init_x);
  auto init_u_traj = PiecewisePolynomial<double>::ZeroOrderHold(init_time, init_u);

  //Initialize force trajectories
  for (int j = 0; j < 2; j++) {    
    std::vector<MatrixXd> init_l_j;
    std::vector<MatrixXd> init_lc_j;
    std::vector<MatrixXd> init_vc_j;
    std::vector<double> init_time_j;
    for (int i = 0; i < N; i++) {
      init_time_j.push_back(i*.2);
      init_l_j.push_back(init_l_vec);
      init_lc_j.push_back(init_l_vec);
      init_vc_j.push_back(VectorXd::Zero(2));
    }

    auto init_l_traj_j = PiecewisePolynomial<double>::ZeroOrderHold(init_time_j,init_l_j);
    auto init_lc_traj_j = PiecewisePolynomial<double>::ZeroOrderHold(init_time_j,init_lc_j);
    auto init_vc_traj_j = PiecewisePolynomial<double>::ZeroOrderHold(init_time_j,init_vc_j);

    init_l_traj.push_back(init_l_traj_j);
    init_lc_traj.push_back(init_lc_traj_j);
    init_vc_traj.push_back(init_vc_traj_j);
  }

  if (FLAGS_autodiff) {
    std::unique_ptr<MultibodyPlant<drake::AutoDiffXd>> plant_autodiff =
        drake::systems::System<double>::ToAutoDiffXd(*plant);
    auto prog = dairlib::runDircon<drake::AutoDiffXd>(
      std::move(plant_autodiff), plant.get(), std::move(scene_graph),
      FLAGS_strideLength, FLAGS_duration, init_x_traj, init_u_traj, init_l_traj,
      init_lc_traj, init_vc_traj);
  } else {
    auto prog = dairlib::runDircon<double>(
      std::move(plant), plant.get(), std::move(scene_graph),
      FLAGS_strideLength, FLAGS_duration, init_x_traj, init_u_traj, init_l_traj,
      init_lc_traj, init_vc_traj);
  }
}

