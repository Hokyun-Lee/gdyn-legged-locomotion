#include <legged_controllers/virtaul_spring_damper_controller.h>

void VirtualSpringDamperController::init()
{
    for (int i=0; i<4; i++)
	{
        // leg stiffness and damping
        _kp_leg[i].resize(3);
        _kd_leg[i].resize(3);

        _kp_leg[i](0) = 3000.0;
        _kp_leg[i](1) = 3000.0;
        _kp_leg[i](2) = 3000.0;

        _kd_leg[i](0) = 300.0;
        _kd_leg[i](1) = 300.0;
        _kd_leg[i](2) = 300.0;
    }

    // hip position offset from base to hip joint
    _x_offset[0].p(0) = 0.3735;  
    _x_offset[0].p(1) = 0.207;

    _x_offset[1].p(0) = 0.3735;  
    _x_offset[1].p(1) = -0.207;

    _x_offset[2].p(0) = -0.3735;  
    _x_offset[2].p(1) = 0.207;

    _x_offset[3].p(0) = -0.3735;  
    _x_offset[3].p(1) = -0.207;  
  
}

void VirtualSpringDamperController::setControlInput(const std::array<KDL::Vector, 4>& p_leg, 
                                                    const std::array<Eigen::Vector3d, 4>& v_leg, 
                                                    const std::array<KDL::JntArray, 4>& G_leg)
{
    _p_leg = p_leg;
    _v_leg = v_leg;
    _G_leg = G_leg;
}

void VirtualSpringDamperController::getControlOutput(std::array<Eigen::Vector3d, 4>& F_leg)
{
    F_leg = _F_leg;
}

void VirtualSpringDamperController::compute()
{
    for (int i = 0; i < 4; i++)
    {
        // desired foot position/velocity form hip joint
        _xd[i].p(0) = 0.0; 
        _xd[i].p(1) = 0.0; 
        _xd[i].p(2) = -0.4;

        _xd_dot[i].vel(0) = 0.0;
        _xd_dot[i].vel(1) = 0.0;
        _xd_dot[i].vel(2) = 0.0;

        // convert hip position decription from base to hip joint
        _p_leg[i](0) = _p_leg[i](0) - _x_offset[i].p(0);
        _p_leg[i](1) = _p_leg[i](1) - _x_offset[i].p(1);

        // virtual spring-damper controller
        _F_leg[i](0) = _kp_leg[i](0)*(_xd[i].p(0) - _p_leg[i](0)) + _kd_leg[i](0)*(_xd_dot[i].vel(0) - _v_leg[i](0)) + _G_leg[i](0);
        _F_leg[i](1) = _kp_leg[i](1)*(_xd[i].p(1) - _p_leg[i](1)) + _kd_leg[i](1)*(_xd_dot[i].vel(1) - _v_leg[i](1)) + _G_leg[i](1);
        _F_leg[i](2) = _kp_leg[i](2)*(_xd[i].p(2) - _p_leg[i](2)) + _kd_leg[i](2)*(_xd_dot[i].vel(2) - _v_leg[i](2)) + _G_leg[i](2);
    }
}

