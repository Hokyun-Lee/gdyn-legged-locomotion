#pragma once

#include <fstream>
#include <iostream>
using namespace std;

#define MPC_Debugging

#include <array>
#include <utility/math_func.h>

// kdl
#include <kdl/tree.hpp>
#include <kdl/kdl.hpp>
#include <kdl/chain.hpp>
#include <kdl/chaindynparam.hpp>
#include <kdl_parser/kdl_parser.hpp>

// qpOASES
#include <qpOASES.hpp>

#define SamplingTime 0.001
#define MPC_Step 3

#define L_00_gain 100.0
#define L_11_gain_x 1000.0
#define L_11_gain_y 1000.0
#define L_11_gain_z 4000.0
#define L_22_gain_wx 100.0
#define L_22_gain_wy 100.0
#define L_22_gain_wz 100.0
#define L_33_gain_vx 100.0
#define L_33_gain_vy 100.0
#define L_33_gain_vz 100.0
#define L_44_gain 0.0

#define K_00_gain 0.000000001
#define K_11_gain 0.000000001
#define K_22_gain 0.000000001
#define K_33_gain 0.000000001

#define Force_min 10
#define Force_max 666 

#define Gravity 9.81

class MPCController
{
public:
    MPCController() {}
    
    void init(double m_body, const Eigen::Vector3d& p_com_body, const Eigen::Matrix3d& I_com, double mu);

    void setControlInput(const Eigen::Vector3d& p_body_d, 
            const Eigen::Vector3d& p_body_dot_d, 
            const Eigen::Matrix3d& R_body_d, 
            const Eigen::Vector3d& w_body_d,
            const std::array<Eigen::Vector3d,4>& p_body2leg_d,
			const Eigen::Vector3d& p_body, 
            const Eigen::Vector3d& p_body_dot,
            const Eigen::Matrix3d& R_body,
            const Eigen::Vector3d w_body,
            const std::array<Eigen::Vector3d,4>& p_body2leg);            

    void getControlOutput(std::array<Eigen::Vector3d, 4>& F_leg);

    void update();

public:
    // parameter
    double _m_body;
    Eigen::Vector3d _p_body2com;
    Eigen::Matrix3d _I_com;

    double _mu; // friction coefficient on ground  

    // command and state
    Eigen::Vector3d _p_com_d, _p_com_dot_d, _p_com, _p_com_dot;
    std::array<Eigen::Vector3d, 4> _p_leg, _p_leg_d, _F_leg;

    Eigen::Matrix3d _R_body_d, _R_body;   // have to change angle-axis or quaternion

    Eigen::Vector3d _w_body_d, _w_body;

    // optimization output
    Eigen::Matrix<double, 12, 1> _F;

    // Define Parameter For MPC Controller
    Eigen::MatrixXd _A_c, _A_d, _B_c, _B_c_d, _B_d, _B_d_d;
    Eigen::MatrixXd _I3x3, _I15x15;
    Eigen::MatrixXd _I_hat;
    Eigen::MatrixXd _p_leg_1_skew, _p_leg_2_skew, _p_leg_3_skew, _p_leg_4_skew;
    Eigen::MatrixXd _p_leg_1_skew_d, _p_leg_2_skew_d, _p_leg_3_skew_d, _p_leg_4_skew_d;
    Eigen::MatrixXd _T15X15, _T12X12;
    Eigen::MatrixXd _A_qp, _B_qp, _Temp; 
    Eigen::MatrixXd _L_d,  _K_d;
    Eigen::MatrixXd _H_qp, _L_qp, _K_qp;   
    Eigen::MatrixXd _g_qp, _x0, _xref, _xref_qp;  

    Eigen::MatrixXd _C_1leg, _C_4leg, _C_qp;
    Eigen::MatrixXd _lbC_1leg, _lbC_4leg, _lbC_qp;
    Eigen::MatrixXd _ub_1leg, _ub_4leg, _ub_qp;
    Eigen::MatrixXd _lb_1leg, _lb_4leg, _lb_qp;      
};