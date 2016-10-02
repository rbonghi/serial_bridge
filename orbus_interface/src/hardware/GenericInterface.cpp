#include "hardware/GenericInterface.h"

namespace ORInterface
{

GenericInterface::GenericInterface(const ros::NodeHandle &nh, const ros::NodeHandle &private_nh, orbus::serial_controller *serial)
    : DiagnosticTask("board")
    , mNh(nh)
    , private_mNh(private_nh)
    , mSerial(serial)
    , serial_status(true)
    , code_date("Unknown"), code_version("Unknown"), code_author("Unknown"), code_board_type("Unknown"), code_board_name("Unknown")
{
    bool initCallback = mSerial->addCallback(&GenericInterface::systemFrame, this, HASHMAP_SYSTEM);

    //Publisher
    pub_time = private_mNh.advertise<orbus_interface::BoardTime>("system", 10,
                boost::bind(&GenericInterface::connectCallback, this, _1));
    //Services
    srv_board = private_mNh.advertiseService("system", &GenericInterface::service_Callback, this);

    // Build a packet
    packet_information_t frame_code_date = CREATE_PACKET_RESPONSE(SYSTEM_CODE_DATE, HASHMAP_SYSTEM, PACKET_REQUEST);
    packet_information_t frame_code_version = CREATE_PACKET_RESPONSE(SYSTEM_CODE_VERSION, HASHMAP_SYSTEM, PACKET_REQUEST);
    packet_information_t frame_code_author = CREATE_PACKET_RESPONSE(SYSTEM_CODE_AUTHOR, HASHMAP_SYSTEM, PACKET_REQUEST);
    packet_information_t frame_code_board_type = CREATE_PACKET_RESPONSE(SYSTEM_CODE_BOARD_TYPE, HASHMAP_SYSTEM, PACKET_REQUEST);
    packet_information_t frame_code_board_name = CREATE_PACKET_RESPONSE(SYSTEM_CODE_BOARD_NAME, HASHMAP_SYSTEM, PACKET_REQUEST);

    if(mSerial->addFrame(frame_code_date)->addFrame(frame_code_version)->addFrame(frame_code_author)->addFrame(frame_code_board_type)->addFrame(frame_code_board_name)->sendList())
    {
        ROS_DEBUG_STREAM("Send Service information messages");
    }
    else
    {
        ROS_ERROR_STREAM("Any messages from board");
    }
}

void GenericInterface::initializeDiagnostic() {

    ROS_INFO_STREAM("Name board: " << code_board_name << " - " << code_version);
    diagnostic_updater.setHardwareID(code_board_name);

    // Initialize this diagnostic interface
    diagnostic_updater.add(*this);
}

void GenericInterface::run(diagnostic_updater::DiagnosticStatusWrapper &stat) {
    ROS_DEBUG_STREAM("DIAGNOSTIC Generic interface I'm here!");
    // Build a packet
    packet_information_t frame = CREATE_PACKET_RESPONSE(SYSTEM_TIME, HASHMAP_SYSTEM, PACKET_REQUEST);
    // Add packet in the frame
    if(mSerial->addFrame(frame)->sendList())
    {
        ROS_DEBUG_STREAM("Request Diagnostic COMPLETED");
    }
    else
    {
        ROS_ERROR_STREAM("Unable to receive packet from uNav");
    }

    stat.add("Name board", code_board_name);
    stat.add("Type board", code_board_type);
    stat.add("Author", code_author);
    stat.add("Version", code_version);
    stat.add("Build", code_date);

    stat.add("Idle (%)", (int) msg.idle);
    stat.add("ADC (nS)", (int) msg.ADC);
    stat.add("LED (nS)", (int) msg.led);
    stat.add("Serial parser (nS)", (int) msg.serial_parser);
    stat.add("I2C (nS)", (int) msg.I2C);

    stat.summary(diagnostic_msgs::DiagnosticStatus::OK, "Board ready!");
}

void GenericInterface::connectCallback(const ros::SingleSubscriberPublisher& pub) {
    ROS_INFO("Connect: %s - %s", pub.getSubscriberName().c_str(), pub.getTopic().c_str());
}

void GenericInterface::systemFrame(unsigned char option, unsigned char type, unsigned char command, message_abstract_u message) {
    ROS_DEBUG_STREAM("Frame [Option: " << option << ", HashMap: " << type << ", Command: " << command << "]");
    switch (command) {
    case SYSTEM_CODE_DATE:
        code_date = string((char*)message.system.service);
        break;
    case SYSTEM_CODE_VERSION:
        code_version = string((char*)message.system.service);
        break;
    case SYSTEM_CODE_AUTHOR:
        code_author = string((char*)message.system.service);
        break;
    case SYSTEM_CODE_BOARD_TYPE:
        code_board_type = string((char*)message.system.service);
        break;
    case SYSTEM_CODE_BOARD_NAME:
        code_board_name = string((char*)message.system.service);
        break;
    case SYSTEM_TIME:
        msg.idle = message.system.time.idle;
        msg.ADC = message.system.time.adc;
        msg.led = message.system.time.led;
        msg.serial_parser = message.system.time.parser;
        msg.I2C = message.system.time.i2c;
        // publish a message
        msg.header.stamp = ros::Time::now();
        pub_time.publish(msg);
        break;
    default:
        ROS_ERROR_STREAM("System message \""<< command << "\"=(" << (int) command << ")" << " does not implemented!");
        break;
    }
}

bool GenericInterface::service_Callback(orbus_interface::Service::Request &req, orbus_interface::Service::Response &msg) {
    // Convert to lower case
    std::transform(req.service.begin(), req.service.end(), req.service.begin(), ::tolower);
    //ROS_INFO_STREAM("Name request: " << req.service);
    if(req.service.compare("info") == 0)
    {
        msg.information = "\nName board: " + code_board_name + "\n"
                          "Board type: " + code_board_type + "\n"
                          "Author: " + code_author + "\n"
                          "Version: " + code_version + "\n"
                          "Build: " + code_date + "\n";
    }
    else if(req.service.compare("reset") == 0)
    {
        packet_information_t frame_reset = CREATE_PACKET_RESPONSE(SYSTEM_RESET, HASHMAP_SYSTEM, PACKET_REQUEST);
        mSerial->addFrame(frame_reset)->sendList();
    }
    else
    {
        msg.information = "\n List of commands availabes: \n"
                          "* info  - information about this board \n"
                          "* reset - software reset of " + code_board_name + "\n"
                          "* help  - this help.";
    }
    return true;
}

}
