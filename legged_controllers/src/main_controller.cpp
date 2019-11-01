#include <legged_controllers/main_controller.h>

using namespace legged_controllers;

namespace legged_controllers
{

bool MainController::init(hardware_interface::EffortJointInterface* hw, ros::NodeHandle &n)
{	
	_loop_count = 0;
	_node_ptr = &n;

	// List of controlled joints
	if (!n.getParam("joints", _joint_names))
	{
		ROS_ERROR("Could not find joint name");
		return false;
	}
	_n_joints = _joint_names.size();

	if(_n_joints == 0)
	{
		ROS_ERROR("List of joint names is empty.");
		return false;
	}
	else
	{
		ROS_INFO("Find %d joints", _n_joints);
	}

	// urdf
	urdf::Model urdf;
	if (!urdf.initParam("robot_description"))
	{
		ROS_ERROR("Failed to parse urdf file");
		return false;
	}

	// joint handle
	for(int i=0; i<_n_joints; i++)
	{
		try
		{
			_joints.push_back(hw->getHandle(_joint_names[i]));
		}
		catch (const hardware_interface::HardwareInterfaceException& e)
		{
			ROS_ERROR_STREAM("Exception thrown: " << e.what());
			return false;
		}

		urdf::JointConstSharedPtr joint_urdf = urdf.getJoint(_joint_names[i]);
		if (!joint_urdf)
		{
			ROS_ERROR("Could not find joint '%s' in urdf", _joint_names[i].c_str());
			return false;
		}
		_joint_urdfs.push_back(joint_urdf); 
	}

	// kdl parser
  if (!kdl_parser::treeFromUrdfModel(urdf, _robot._kdl_tree)){
		ROS_ERROR("Failed to construct kdl tree");
		return false;
	}

  _robot.init();

	// command and state (12x1)
	_tau_d.data = Eigen::VectorXd::Zero(_n_joints);
	_tau_fric.data = Eigen::VectorXd::Zero(_n_joints);
	_qdot_d.data = Eigen::VectorXd::Zero(_n_joints);
	_qddot_d.data = Eigen::VectorXd::Zero(_n_joints);
	_q_d_old.data = Eigen::VectorXd::Zero(_n_joints);
	_qdot_d_old.data = Eigen::VectorXd::Zero(_n_joints);
	
	_q_error.data = Eigen::VectorXd::Zero(_n_joints);
	_qdot_error.data = Eigen::VectorXd::Zero(_n_joints);



	// gains
	_kp.resize(_n_joints);
	_kd.resize(_n_joints);

	// service updating gain
	_gains_kp_buffer.writeFromNonRT(std::vector<double>(_n_joints, 0.0));
	_gains_kd_buffer.writeFromNonRT(std::vector<double>(_n_joints, 0.0));
	_update_gain_srv = n.advertiseService("update_gain", &MainController::updateGain, this);

	updateGain();

	// command subscriber
	_commands_buffer.writeFromNonRT(std::vector<double>(_n_joints, 0.0));
	_commands_sub = n.subscribe<std_msgs::Float64MultiArray>("command", 1, &MainController::subscribeCommand, this);

	// state subscriber
	_trunk_state_buffer.writeFromNonRT(Trunk());
  _link_states_sub = n.subscribe("/gazebo/link_states", 1, &MainController::subscribeTrunkState, this);
  for (int i=0; i<4; i++)
    _contact_states_buffer[i].writeFromNonRT(false);

  _contact_states_sub[0] = n.subscribe("/hyq/lf_foot_bumper", 1, &MainController::subscribeLFContactState, this);
  _contact_states_sub[1] = n.subscribe("/hyq/rf_foot_bumper", 1, &MainController::subscribeRFContactState, this);
  _contact_states_sub[2] = n.subscribe("/hyq/lh_foot_bumper", 1, &MainController::subscribeLHContactState, this);
  _contact_states_sub[3] = n.subscribe("/hyq/rh_foot_bumper", 1, &MainController::subscribeRHContactState, this);

	// start realtime state publisher
	_controller_state_pub.reset(
		new realtime_tools::RealtimePublisher<legged_controllers::ControllerJointState>(n, "state", 1));

	_controller_state_pub->msg_.header.stamp = ros::Time::now();
	for (size_t i=0; i<_n_joints; i++)
	{
		_controller_state_pub->msg_.name.push_back(_joint_names[i]);
		_controller_state_pub->msg_.command.push_back(0.0);
		_controller_state_pub->msg_.command_dot.push_back(0.0);
		_controller_state_pub->msg_.state.push_back(0.0);
		_controller_state_pub->msg_.state_dot.push_back(0.0);
		_controller_state_pub->msg_.q_error.push_back(0.0);
		_controller_state_pub->msg_.qdot_error.push_back(0.0);
		_controller_state_pub->msg_.effort_command.push_back(0.0);
		_controller_state_pub->msg_.effort_feedforward.push_back(0.0);
		_controller_state_pub->msg_.effort_feedback.push_back(0.0);
	}

  // first controller
  _robot.setController(4, quadruped_robot::controllers::VirtualSpringDamper);

	// virtaul spring damper controller
	_virtual_spring_damper_controller.init();

	// balance controller
  _robot._m_body = 83.282; //60.96, 71.72,
  _robot._mu_foot = 0.6;	// TO DO: get this value from robot model
  _robot._I_com_body = Eigen::Matrix3d::Zero();
  _robot._I_com_body.diagonal() << 1.5725937, 8.5015928, 9.1954911;
  _robot._p_body2com = Eigen::Vector3d(0.056, 0.0215, 0.00358);

  _balance_controller.init();
  _mpc_controller.init(_robot._m_body, _robot._p_body2com, _robot._I_com_body, _robot._mu_foot);
	
	return true;
}

void MainController::starting(const ros::Time& time)
{
	_t = 0;
	
	ROS_INFO("Starting Leg Controller");
}

void MainController::subscribeCommand(const std_msgs::Float64MultiArrayConstPtr& msg)
{
	if(msg->data.size()!=_n_joints)
	{ 
    ROS_ERROR_STREAM("Dimension of command (" << msg->data.size() << ") does not match number of joints (" << _n_joints << ")! Not executing!");
	return; 
	}
	_commands_buffer.writeFromNonRT(msg->data);
}

void MainController::subscribeTrunkState(const gazebo_msgs::LinkStatesConstPtr& msg)
{
	Trunk trunk;

	trunk._p = Eigen::Vector3d(msg->pose[1].position.x,	msg->pose[1].position.y, msg->pose[1].position.z);
	trunk._v = Eigen::Vector3d(msg->twist[1].linear.x, msg->twist[1].linear.y, msg->twist[1].linear.z);
	trunk._o = Eigen::Quaterniond(Eigen::Quaterniond(msg->pose[1].orientation.w,
														msg->pose[1].orientation.x,
														msg->pose[1].orientation.y,
														msg->pose[1].orientation.z));
	trunk._w = Eigen::Vector3d(msg->twist[1].angular.x,
		msg->twist[1].angular.y, msg->twist[1].angular.z);

	_trunk_state_buffer.writeFromNonRT(trunk);
}

void MainController::subscribeLFContactState(const gazebo_msgs::ContactsStateConstPtr& msg)
{
  Eigen::Vector3d contact_force;

  if (msg->states.size() > 0)
    contact_force << msg->states[0].total_wrench.force.x, msg->states[0].total_wrench.force.y, msg->states[0].total_wrench.force.z;

  bool contact_state = contact_force.norm() > 10;

  _contact_states_buffer[0].writeFromNonRT(contact_state);
}

void MainController::subscribeRFContactState(const gazebo_msgs::ContactsStateConstPtr& msg)
{
  Eigen::Vector3d contact_force;

  if (msg->states.size() > 0)
    contact_force << msg->states[0].total_wrench.force.x, msg->states[0].total_wrench.force.y, msg->states[0].total_wrench.force.z;

  bool contact_state = contact_force.norm() > 10;

  _contact_states_buffer[1].writeFromNonRT(contact_state);
}

void MainController::subscribeLHContactState(const gazebo_msgs::ContactsStateConstPtr& msg)
{
  Eigen::Vector3d contact_force;

  if (msg->states.size() > 0)
    contact_force << msg->states[0].total_wrench.force.x, msg->states[0].total_wrench.force.y, msg->states[0].total_wrench.force.z;

  bool contact_state = contact_force.norm() > 10;

  _contact_states_buffer[2].writeFromNonRT(contact_state);
}

void MainController::subscribeRHContactState(const gazebo_msgs::ContactsStateConstPtr& msg)
{
  Eigen::Vector3d contact_force;

  if (msg->states.size() > 0)
    contact_force << msg->states[0].total_wrench.force.x, msg->states[0].total_wrench.force.y, msg->states[0].total_wrench.force.z;

  bool contact_state = contact_force.norm() > 10;

  _contact_states_buffer[3].writeFromNonRT(contact_state);
}

bool MainController::updateGain(UpdateGain::Request& request, UpdateGain::Response& response)
{
	updateGain();
}

bool MainController::updateGain()
{
	std::vector<double> kp(_n_joints), kd(_n_joints);
	std::string gain_name;

	for (size_t i = 0; i < _n_joints; i++)
	{
		// FIXME. generalize name of robot
		gain_name = "/hyq/main_controller/gains/" + _joint_names[i] + "/p";
		if (_node_ptr->getParam(gain_name, kp[i]))
		{
			ROS_INFO("Update gain %s = %.2f", gain_name.c_str(), kp[i]);
		}
		else
		{
			ROS_ERROR("Cannot find %s gain", gain_name.c_str());
			return false;
		}

		gain_name = "/hyq/main_controller/gains/" + _joint_names[i] + "/d";
		if (_node_ptr->getParam(gain_name, kd[i]))
		{
			ROS_INFO("Update gain %s = %.2f", gain_name.c_str(), kd[i]);
		}
		else
		{
			ROS_ERROR("Cannot find %s gain", gain_name.c_str());
			return false;
		}
	}

	_gains_kp_buffer.writeFromNonRT(kp);
	_gains_kd_buffer.writeFromNonRT(kd);

	return true;
}

bool MainController::srvMoveBodyCB(MoveBody::Request& request, MoveBody::Response& response)
{
	// Eigen::Vector6d delta_pose = Map<Eigen::Vector6d>(request.delta_pose);
	
	// p_body_d = p_body_d + delta_pose.head(3) * MM2M;

	// AngleAxisd request.delta_pose[3] * D2R;

	// _minjerk_traj.setTrajInput(request.delta_pose, request.duration);
}

void MainController::update(const ros::Time& time, const ros::Duration& period)
{
  // Update from real-time buffer
	std::vector<double> & commands = *_commands_buffer.readFromRT();
	std::vector<double> & kp = *_gains_kp_buffer.readFromRT();
	std::vector<double> & kd = *_gains_kd_buffer.readFromRT();
	Trunk& trunk_state = *_trunk_state_buffer.readFromRT();
	std::array<bool, 4> contact_states;


  for (int i=0; i<4; i++)  {
    contact_states[i] = *_contact_states_buffer[i].readFromRT();
	}

	double dt = period.toSec();
	_t += dt;

	// update state from tree (12x1) to each leg (4x3)
	std::array<Eigen::Vector3d, 4> q_leg, qdot_leg;
	for (size_t i = 0; i < _n_joints; i++)
	{
		if (i < 3)
		{
      q_leg[0](i) = _joints[i].getPosition();
			qdot_leg[0](i) = _joints[i].getVelocity();
		}
		else if (i < 6)
		{
      q_leg[1](i-3) = _joints[i].getPosition();
			qdot_leg[1](i-3) = _joints[i].getVelocity();
		}
		else if (i < 9)
		{
      q_leg[2](i-6) = _joints[i].getPosition();
			qdot_leg[2](i-6) = _joints[i].getVelocity();
		}
		else
		{
      q_leg[3](i-9) = _joints[i].getPosition();
			qdot_leg[3](i-9) = _joints[i].getVelocity();
		}
	}

  // Sensor Data - continuosly update, subscribe from gazebo for now, @TODO: get this as raw sensor data
  _robot.updateSensorData(q_leg, qdot_leg, Pose(trunk_state._p, trunk_state._o), PoseVel(trunk_state._v, trunk_state._w), contact_states);

  // @TODO: State Estimation, For now, use gazebo data
  //  _state_estimation.update(_robot);

  // @TODO: Trajectory Generation, update trajectory - get from this initial state(temporary)
  // _trajectory_generator.update(_robot);

	static int td = 0;
	if (td++ == 5000)
	{
    _robot._pose_body_d._pos = _robot._pose_body._pos;
    _robot.setController(4, quadruped_robot::controllers::QP_Balancing);

    ROS_INFO("Change Controller from virtual spring damper to qp balance");
	}

  _robot._pose_body_d._rot_quat.setIdentity();
  _robot._pose_vel_body_d._angular.setZero();

  // Kinematics, Dynamics
  _robot.calKinematicsDynamics();

#ifdef MPC_Debugging

	static int count=0;
	count++;

	static int count_p = 0;
	count_p++;

	if(count < 5000)
	{
		if(count_p>100)
		{
			printf("############ Running Vitual Spring Damper Controller\n");
			count_p = 0;
		}

    _virtual_spring_damper_controller.setControlInput(_p_leg,_v_leg,_G_leg);
		_virtual_spring_damper_controller.compute();
		_virtual_spring_damper_controller.getControlOutput(_F_leg);

		p_body_d = p_body;
		p_body_dot_d = p_body_dot;
		R_body_d = R_body;
		w_body_d = w_body;
		_p_leg_d = _p_leg;
	}
	else
	{
		if(count_p>100)
		{
			printf("############ Running MPC Controller\n");
			count_p = 0;
		}

		_mpc_controller.setControlInput(p_body_d, p_body_dot_d, R_body_d, w_body_d, _p_leg_d,
										p_body, p_body_dot, R_body, w_body, _p_leg);
		_mpc_controller.update();
		_mpc_controller.getControlOutput(_F_leg_balance);
		_F_leg = _F_leg_balance;

		print_state();
	}

#else
  _virtual_spring_damper_controller.calControlInput(_robot, _F_leg);
  _balance_controller.calControlInput(_robot, _F_leg);
#endif

  // Convert force to torque
	for (size_t i=0; i<4; i++)	{
    _tau_leg[i] = _robot._Jv_leg[i].transpose() * _F_leg[i];
	}

	// torque command
	for(int i=0; i<_n_joints; i++)
	{
		if (i < 3)
			_tau_d(i) = _tau_leg[0](i);
		else if (i < 6)
			_tau_d(i) = _tau_leg[1](i-3);
		else if (i < 9)
			_tau_d(i) = _tau_leg[2](i-6);
		else
			_tau_d(i) = _tau_leg[3](i-9);
	}

	for(int i=0; i<_n_joints; i++)
	{
		// effort saturation
    if (_tau_d(i) >= _joint_urdfs[i]->limits->effort)
    {
//       ROS_INFO("effort saturation + %d", i);
      _tau_d(i) = _joint_urdfs[i]->limits->effort;
    }
		
    if (_tau_d(i) <= -_joint_urdfs[i]->limits->effort)
    {
//      ROS_INFO("effort saturation - %d", i);
      _tau_d(i) = -_joint_urdfs[i]->limits->effort;
    }

		_joints[i].setCommand( _tau_d(i) );
	}

	// publish
	if (_loop_count % 10 == 0)
	{
		if (_controller_state_pub->trylock())
		{
			_controller_state_pub->msg_.header.stamp = time;
			for(int i=0; i<_n_joints; i++)
			{
//				_controller_state_pub->msg_.command[i] = R2D*_q_d(i);
//				_controller_state_pub->msg_.command_dot[i] = R2D*_qdot_d(i);
//				_controller_state_pub->msg_.state[i] = R2D*_q(i);
//				_controller_state_pub->msg_.state_dot[i] = R2D*_qdot(i);
//				_controller_state_pub->msg_.q_error[i] = R2D*_q_error(i);
//				_controller_state_pub->msg_.qdot_error[i] = R2D*_qdot_error(i);
				// FIXME. temporarily
				// _controller_state_pub->msg_.effort_command[i] = _tau_d(i);
				// _controller_state_pub->msg_.effort_feedback[i] = _tau_d(i) - _controller_state_pub->msg_.effort_feedforward[i];
			}

			for(int i=0; i<4; i++)
			{
				for(int j=0; j<3; j++)
				{
					_controller_state_pub->msg_.effort_command[i*3+j] = _F_leg[i](j);
					_controller_state_pub->msg_.effort_feedback[i*3+j] = _F_leg_balance[i](j);
				}
			}
			_controller_state_pub->unlockAndPublish();
		}
	}

    // ********* printf state *********
  //printState();
}

void MainController::enforceJointLimits(double &command, unsigned int index)
{
	// Check that this joint has applicable limits
	if (_joint_urdfs[index]->type == urdf::Joint::REVOLUTE || _joint_urdfs[index]->type == urdf::Joint::PRISMATIC)
	{
		if (command > _joint_urdfs[index]->limits->upper) // above upper limnit
		{
			command = _joint_urdfs[index]->limits->upper;
		}
		else if (command < _joint_urdfs[index]->limits->lower) // below lower limit
		{
			command = _joint_urdfs[index]->limits->lower;
		}
	}
}

void MainController::printState()
{
	static int count = 0;
	if (count > 100)
	{
		printf("*********************************************************\n\n");
		printf("*** Simulation Time (unit: sec)  ***\n");
		printf("t = %f\n", _t);
		printf("\n");

		printf("*********************** Left Front Leg **********************************\n\n");

		//printf("*** Actual Position in Joint Space (unit: deg) ***\n");
		//printf("q(1): %f, ", _q_leg[0](0) * R2D);
		//printf("q(2): %f, ", _q_leg[0](1) * R2D);
		//printf("q(3): %f, ", _q_leg[0](2) * R2D);
		//printf("\n");
		//printf("\n");

		//printf("*** Actual Position in Task Space (unit: m) ***\n");
		//printf("x(1): %f, ", _p_leg[0](0) * 1);
		//printf("x(2): %f, ", _p_leg[0](1) * 1);
		//printf("x(3): %f, ", _p_leg[0](2) * 1);
		//printf("\n");
		//printf("\n");

		//printf("*** Virtual Leg Forces (unit: N) ***\n");
		//printf("X Force Input: %f, ", _F_leg[0](0));
		//printf("Y Force Input: %f, ", _F_leg[0](1));
		//printf("Z Force Input: %f, ", _F_leg[0](2));
		//printf("\n");
		//printf("\n");

		printf("*** Balance Leg Forces (unit: N) ***\n");
		printf("X Force Input: %f, ", _F_leg[0](0));
		printf("Y Force Input: %f, ", _F_leg[0](1));
		printf("Z Force Input: %f, ", _F_leg[0](2));
		printf("\n");
		printf("\n");

		printf("*** Torque Input (unit: Nm) ***\n");
		printf("Hip AA Input: %f, ", _tau_leg[0](0));
		printf("Hip FE Input: %f, ", _tau_leg[0](1));
		printf("Knee FE Input: %f, ", _tau_leg[0](2));
		printf("\n");
		printf("\n");

		// printf("*** Gravity Compensation Torque Input(unit: Nm) ***\n");
		// printf("Hip AA Input: %f, ", _G_leg[0](0));
		// printf("Hip FE Input: %f, ", _G_leg[0](1));
		// printf("Knee FE Input: %f, ", _G_leg[0](2));
		// printf("\n");
		// printf("\n");

		printf("*********************** Right Front Leg **********************************\n\n");
		//printf("*** Actual Position in Joint Space (unit: deg) ***\n");
		//printf("q(1): %f, ", _q_leg[1](0) * R2D);
		//printf("q(2): %f, ", _q_leg[1](1) * R2D);
		//printf("q(3): %f, ", _q_leg[1](2) * R2D);
		//printf("\n");
		//printf("\n");

		//printf("*** Actual Position in Task Space (unit: m) ***\n");
		//printf("x(1): %f, ", _p_leg[1](0) * 1);
		//printf("x(2): %f, ", _p_leg[1](1) * 1);
		//printf("x(3): %f, ", _p_leg[1](2) * 1);
		//printf("\n");
		//printf("\n");

		//printf("*** Virtual Leg Forces (unit: N) ***\n");
		//printf("X Force Input: %f, ", _F_leg[1](0));
		//printf("Y Force Input: %f, ", _F_leg[1](1));
		//printf("Z Force Input: %f, ", _F_leg[1](2));
		//printf("\n");
		//printf("\n");

		printf("*** Balance Leg Forces (unit: N) ***\n");
		printf("X Force Input: %f, ", _F_leg[1](0));
		printf("Y Force Input: %f, ", _F_leg[1](1));
		printf("Z Force Input: %f, ", _F_leg[1](2));
		printf("\n");
		printf("\n");

		printf("*** Torque Input (unit: Nm) ***\n");
		printf("Hip AA Input: %f, ", _tau_leg[1](0));
		printf("Hip FE Input: %f, ", _tau_leg[1](1));
		printf("Knee FE Input: %f, ", _tau_leg[1](2));
		printf("\n");
		printf("\n");

		// printf("*** Gravity Compensation Torque Input(unit: Nm) ***\n");
		// printf("Hip AA Input: %f, ", _G_leg[1](0));
		// printf("Hip FE Input: %f, ", _G_leg[1](1));
		// printf("Knee FE Input: %f, ", _G_leg[1](2));
		// printf("\n");
		// printf("\n");

		printf("*********************** Left Hind Leg **********************************\n\n");
		//printf("*** Actual Position in Joint Space (unit: deg) ***\n");
		//printf("q(1): %f, ", _q_leg[2](0) * R2D);
		//printf("q(2): %f, ", _q_leg[2](1) * R2D);
		//printf("q(3): %f, ", _q_leg[2](2) * R2D);
		//printf("\n");
		//printf("\n");

		//printf("*** Actual Position in Task Space (unit: m) ***\n");
		//printf("x(1): %f, ", _p_leg[2](0) * 1);
		//printf("x(2): %f, ", _p_leg[2](1) * 1);
		//printf("x(3): %f, ", _p_leg[2](2) * 1);
		//printf("\n");
		//printf("\n");

		//printf("*** Virtual Leg Forces (unit: N) ***\n");
		//printf("X Force Input: %f, ", _F_leg[2](0));
		//printf("Y Force Input: %f, ", _F_leg[2](1));
		//printf("Z Force Input: %f, ", _F_leg[2](2));
		//printf("\n");
		//printf("\n");

		printf("*** Balance Leg Forces (unit: N) ***\n");
		printf("X Force Input: %f, ", _F_leg[2](0));
		printf("Y Force Input: %f, ", _F_leg[2](1));
		printf("Z Force Input: %f, ", _F_leg[2](2));
		printf("\n");
		printf("\n");

		printf("*** Torque Input (unit: Nm) ***\n");
		printf("Hip AA Input: %f, ", _tau_leg[2](0));
		printf("Hip FE Input: %f, ", _tau_leg[2](1));
		printf("Knee FE Input: %f, ", _tau_leg[2](2));
		printf("\n");
		printf("\n");

		// printf("*** Gravity Compensation Torque Input(unit: Nm) ***\n");
		// printf("Hip AA Input: %f, ", _G_leg[2](0));
		// printf("Hip FE Input: %f, ", _G_leg[2](1));
		// printf("Knee FE Input: %f, ", _G_leg[2](2));
		// printf("\n");
		// printf("\n");

		printf("*********************** Right Hind Leg **********************************\n\n");
		//printf("*** Actual Position in Joint Space (unit: deg) ***\n");
		//printf("q(1): %f, ", _q_leg[3](0) * R2D);
		//printf("q(2): %f, ", _q_leg[3](1) * R2D);
		//printf("q(3): %f, ", _q_leg[3](2) * R2D);
		//printf("\n");
		//printf("\n");

		//printf("*** Actual Position in Task Space (unit: m) ***\n");
		//printf("x(1): %f, ", _p_leg[3](0) * 1);
		//printf("x(2): %f, ", _p_leg[3](1) * 1);
		//printf("x(3): %f, ", _p_leg[3](2) * 1);
		//printf("\n");
		//printf("\n");

		//printf("*** Virtual Leg Forces (unit: N) ***\n");
		//printf("X Force Input: %f, ", _F_leg[3](0));
		//printf("Y Force Input: %f, ", _F_leg[3](1));
		//printf("Z Force Input: %f, ", _F_leg[3](2));
		//printf("\n");
		//printf("\n");

		printf("*** Balance Leg Forces (unit: N) ***\n");
		printf("X Force Input: %f, ", _F_leg[3](0));
		printf("Y Force Input: %f, ", _F_leg[3](1));
		printf("Z Force Input: %f, ", _F_leg[3](2));
		printf("\n");
		printf("\n");

		printf("*** Torque Input (unit: Nm) ***\n");
		printf("Hip AA Input: %f, ", _tau_leg[3](0));
		printf("Hip FE Input: %f, ", _tau_leg[3](1));
		printf("Knee FE Input: %f, ", _tau_leg[3](2));
		printf("\n");
		printf("\n");

		// printf("*** Gravity Compensation Torque Input(unit: Nm) ***\n");
		// printf("Hip AA Input: %f, ", _G_leg[3](0));
		// printf("Hip FE Input: %f, ", _G_leg[3](1));
		// printf("Knee FE Input: %f, ", _G_leg[3](2));
		// printf("\n");
		// printf("\n");

		count = 0;
	}
	count++;
}

} // namespace legged_controllers
PLUGINLIB_EXPORT_CLASS(legged_controllers::MainController, controller_interface::ControllerBase)

