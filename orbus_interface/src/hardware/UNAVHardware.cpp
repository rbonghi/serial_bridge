/* 
 * File:   ROSMotionController.cpp
 * Author: Raffaello Bonghi
 * 
 * Created on 15 November 2013, 18:34
 */

#include "hardware/UNAVHardware.h"
#include <limits>

#include <boost/assign/list_of.hpp>
// Boost header needed:
#include <boost/lexical_cast.hpp>

#include "urdf_parser/urdf_parser.h"

#define NUMBER_PUB 10
#define SGN(x)  ( ((x) < 0) ?  -1 : ( ((x) == 0 ) ? 0 : 1) )

using namespace std;

namespace
{
  const uint8_t LEFT = 0, RIGHT = 1;
}

UNAVHardware::UNAVHardware(const ros::NodeHandle& nh, const ros::NodeHandle &private_nh, ParserPacket* serial, double frequency)
: ORBHardware(nh, private_nh, serial, frequency) {

    /// Verify correct type board
    if (type_board_.compare("Motor Control") != 0) {
        throw (controller_exception("Other board: " + type_board_));
    }

    /// Added all callback to receive information about messages
    serial->addCallback(&UNAVHardware::motorPacket, this, HASHMAP_MOTOR);
    addParameterPacketRequest(&UNAVHardware::addParameter, this);

    /// Load all parameters
    loadParameter();

    /// Load diagnostic
    initializeDiagnostics();

    /// Register all control interface avaiable
    registerControlInterfaces();

}

UNAVHardware::~UNAVHardware() {
    serial_->clearCallback(HASHMAP_MOTION);
    serial_->clearCallback(HASHMAP_MOTOR);
    clearParameterPacketRequest();
}

void UNAVHardware::initializeDiagnostics() {
    for (unsigned int i = 0; i < NUM_MOTORS; i++)
    {
        std::string number_motor_string = "motor_" + boost::lexical_cast<std::string>(i);
        /// Build the diagnostic motor controller
        joints_[i].diagnosticMotor = new DiagnosticMotor(private_nh_, number_motor_string, joints_[i].name);
    }
}

void UNAVHardware::registerControlInterfaces() {
    /// Build harware interfaces
    for (unsigned int i = 0; i < NUM_MOTORS; i++)
    {
        /// Joint hardware interface
        hardware_interface::JointStateHandle joint_state_handle(joints_[i].name,
                                                                &joints_[i].position, &joints_[i].velocity, &joints_[i].effort);
        joint_state_interface_.registerHandle(joint_state_handle);

        /// Differential drive interface
        hardware_interface::JointHandle joint_handle(
                    joint_state_handle, &joints_[i].velocity_command);
        velocity_joint_interface_.registerHandle(joint_handle);

        setupLimits(joint_handle, joints_[i].name, i);

    }
    /// Register interfaces
    registerInterface(&joint_state_interface_);
    registerInterface(&velocity_joint_interface_);

}

void UNAVHardware::setupLimits(hardware_interface::JointHandle joint_handle, std::string name, int i) {
    /// Add a velocity joint limits infomations
    /// Populate with any of the methods presented in the previous example...
    joint_limits_interface::JointLimits limits;
    joint_limits_interface::SoftJointLimits soft_limits;

    // Manual value setting
    limits.has_velocity_limits = true;
    limits.max_velocity = 5.0;

    // Populate (soft) joint limits from URDF
    // Limits specified in URDF overwrite existing values in 'limits' and 'soft_limits'
    // Limits not specified in URDF preserve their existing values
    if(urdf_ != NULL) {
        boost::shared_ptr<const urdf::Joint> urdf_joint = urdf_->getJoint(name);
        const bool urdf_limits_ok = getJointLimits(urdf_joint, limits);
        const bool urdf_soft_limits_ok = getSoftJointLimits(urdf_joint, soft_limits);
        if(urdf_limits_ok) {
            ROS_INFO_STREAM("LOAD " << name << " limits from URDF: " << limits.max_velocity << " rad/s");
        }
        if(urdf_soft_limits_ok) {
            ROS_INFO_STREAM("LOAD " << name << " soft limits from URDF: " << limits.max_velocity << " rad/s");
        }
    }

    // Populate (soft) joint limits from the ros parameter server
    // Limits specified in the parameter server overwrite existing values in 'limits' and 'soft_limits'
    // Limits not specified in the parameter server preserve their existing values
    const bool rosparam_limits_ok = getJointLimits(name, nh_, limits);
    if(rosparam_limits_ok) {
        ROS_INFO_STREAM("LOAD " << name << " limits from ROSPARAM: " << limits.max_velocity << " rad/s");
    }

    // Send joint limits information to board
    motor_t constraint;
    constraint.position = -1;
    constraint.velocity = (motor_control_t) limits.max_velocity*1000;
    constraint.torque = -1;
    motor_command_map_t command;
    command.bitset.motor = i;
    command.bitset.motor = MOTOR_CONSTRAINT;

    packet_t packet_send = serial_->encoder(serial_->createDataPacket(command.command_message, HASHMAP_MOTOR, (message_abstract_u*) & constraint));
    try {
        serial_->sendSyncPacket(packet_send, 3, boost::posix_time::millisec(200));
    } catch (exception &e) {
        ROS_ERROR("%s", e.what());
    }

    joint_limits_interface::VelocityJointSoftLimitsHandle handle(joint_handle, // We read the state and read/write the command
                                                                 limits,       // Limits spec
                                                                 soft_limits);  // Soft limits spec

    vel_limits_interface_.registerHandle(handle);
}

void UNAVHardware::updateDiagnostics() {
    ROS_INFO("Update Diagnostic");
    for(int i = 0; i < NUM_MOTORS; ++i) {
        motor_command_.bitset.motor = i;
        motor_command_.bitset.command = MOTOR_DIAGNOSTIC; ///< Set message to receive diagnostic information
        list_send_.push_back(serial_->createPacket(motor_command_.command_message, PACKET_REQUEST, HASHMAP_MOTOR));
    }

    // Recall default diagnostic
    ORBHardware::updateDiagnostics();
}

void UNAVHardware::updateJointsFromHardware() {
    ROS_INFO("Update Joints");
    for(int i = 0; i < NUM_MOTORS; ++i) {
        motor_command_.bitset.motor = i;
        motor_command_.bitset.command = MOTOR_MEASURE; ///< Set message to receive measure information
        list_send_.push_back(serial_->createPacket(motor_command_.command_message, PACKET_REQUEST, HASHMAP_MOTOR));
    }
}

void UNAVHardware::writeCommandsToHardware(ros::Duration period) {
    //ROS_INFO("Write to Hardware");

    // Enforce joint limits for all registered handles
    // Note: one can also enforce limits on a per-handle basis: handle.enforceLimits(period)
    vel_limits_interface_.enforceLimits(period);

    list_send_.clear();     ///< Clear list of commands
    motor_command_.bitset.command = MOTOR_VEL_REF; ///< Set command to velocity control
    for(int i = 0; i < NUM_MOTORS; ++i) {
        //Build a command message
        motor_command_.bitset.motor = i;
        /// Convert radiant velocity in milliradiant
        long int velocity_long = (long int) joints_[i].velocity_command*1000;
        motor_control_t velocity;
        // >>>>> Saturation on 16 bit values
        if(velocity_long > 32767) {
            velocity = 32767;
        } else if (velocity_long < -32768) {
            velocity_long = -32768;
        } else {
            velocity = (motor_control_t) velocity_long;
        }
        // <<<<< Saturation on 16 bit values
        list_send_.push_back(serial_->createDataPacket(motor_command_.command_message, HASHMAP_MOTOR, (message_abstract_u*) & velocity));
    }
    /// Send message
    try {
        serial_->parserSendPacket(list_send_, 3, boost::posix_time::millisec(200));
    } catch (exception &e) {
        ROS_ERROR("%s", e.what());
    }
}

void UNAVHardware::addParameter(std::vector<packet_information_t>* list_send) {
    motor_command_map_t command;
    std::string number_motor_string;
    for(unsigned int i=0; i < NUM_MOTORS; ++i) {
        command.bitset.motor = i;
        number_motor_string = "motor_" + boost::lexical_cast<std::string>(i);
        /// Name motor
        private_nh_.getParam(number_motor_string + "/name", joints_[i].name);
        /// Check name and path
        //ROS_INFO_STREAM("Path: " << number_motor_string << "/name - Name: " << joints_[i].name);

        /// PIDs for Velocity, effort, position
        joints_[i].configurator_pid_velocity = new MotorPIDConfigurator(private_nh_, number_motor_string, "velocity", i, serial_);
        joints_[i].configurator_pid_effort = new MotorPIDConfigurator(private_nh_, number_motor_string, "effort", i, serial_);
        /// Parameter motor
        joints_[i].configurator_param = new MotorParamConfigurator(private_nh_, number_motor_string, i, serial_);
        /// Emergency motor
        joints_[i].configurator_emergency = new MotorEmergencyConfigurator(private_nh_, number_motor_string, i, serial_);
        /// Reset position motor
        command.bitset.command = MOTOR_POS_RESET;
        motor_control_t reset_coord = 0;
        list_send->push_back(serial_->createDataPacket(command.command_message,HASHMAP_MOTOR, (message_abstract_u*) & reset_coord));
    }

    /// Load URDF from robot_description
    if(nh_.hasParam("/robot_description")) {
        std::string urdf_string;
        nh_.getParam("/robot_description", urdf_string);
        urdf_ = urdf::parseURDF(urdf_string);
    }
}

void UNAVHardware::motorPacket(const unsigned char& command, const message_abstract_u* packet) {
    motor_command_.command_message = command;
    switch (motor_command_.bitset.command) {
    case MOTOR_MEASURE:
        /// Update measure messages
        joints_[motor_command_.bitset.motor].effort = packet->motor.motor.torque;
        joints_[motor_command_.bitset.motor].position += packet->motor.motor.position_delta;
        joints_[motor_command_.bitset.motor].velocity = ((double) packet->motor.motor.velocity) / 1000;
        break;
    case MOTOR_DIAGNOSTIC:
        /// Launch Diagnostic message
        joints_[motor_command_.bitset.motor].diagnosticMotor->run(packet->motor.diagnostic);
        break;
    }
}
