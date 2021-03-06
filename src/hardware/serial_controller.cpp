#include "hardware/serial_controller.h"

namespace orbus
{

serial_controller::serial_controller(string port, unsigned long baudrate)
    : mSerialPort(port)
    , mBaudrate(baudrate)
{
    orb_message_init(&mReceive);           ///< Initialize buffer serial error
    orb_frame_init();                      ///< Initialize hash map packet
    // Start status of the serial controller
    mStatus = SERIAL_OK;
    // Default timeout
    mTimeout = 500;
}

serial_controller::~serial_controller()
{
    stop();
}

bool serial_controller::start()
{
    try
    {
        mSerial.setPort(mSerialPort);
        mSerial.open();
        mSerial.setBaudrate(mBaudrate);

        serial::Timeout to = serial::Timeout::simpleTimeout(mTimeout);
        mSerial.setTimeout(to);
    }
    catch (serial::IOException& e)
    {
        ROS_ERROR_STREAM("Unable to open serial port " << mSerialPort << " - Error: "  << e.what() );
        return false;
    }

    if(mSerial.isOpen()){
        ROS_DEBUG_STREAM("Serial Port correctly initialized: " << mSerialPort );
    }
    else
    {
        ROS_ERROR_STREAM( "Serial port not opened: " << mSerialPort );
        return false;
    }

    mStopping = false;

    if(this->isAlive()){
        ROS_DEBUG_STREAM("ORBUS Connection started: " << mSerialPort );
    }
    else
    {
        ROS_ERROR_STREAM("ORBUS does not found: " << mSerialPort );
        return false;
    }

    ROS_DEBUG_STREAM( "Serial port ready" );
    return true;
}

bool serial_controller::stop()
{
    // Stop the reader
    mStopping = true;
    // Clean all messages
    list_send.clear();
    // Close the serial port
    mSerial.close();
}

bool serial_controller::addCallback(const callback_data_packet_t &callback, unsigned char type)
{
    if (hashmap.find(type) != hashmap.end())
    {
        return false;
    } else
    {
        hashmap[type] = callback;
        return true;
    }

}

serial_controller* serial_controller::addFrame(vector<packet_information_t> packet)
{
    mMutex.lock();
    list_send.reserve(list_send.size() + packet.size());
    list_send.insert(list_send.end(), packet.begin(), packet.end());
    mMutex.unlock();
    return this;
}

serial_controller* serial_controller::addFrame(packet_information_t packet)
{
    mMutex.lock();
    list_send.push_back(packet);
    mMutex.unlock();
    return this;
}

void serial_controller::resetList()
{
    mMutex.lock();
    list_send.clear();
    mMutex.unlock();
}

bool serial_controller::sendList()
{
    mMutex.lock();
    bool state = sendSerialFrame(list_send);
    if(state) {
        list_send.clear();
    }
    mMutex.unlock();
    return state;
}

serial_status_t serial_controller::getStatus()
{
    return mStatus;
}

bool serial_controller::isAlive()
{
    mSerial.flush();
    return sendSerialFrame(CREATE_PACKET_RESPONSE(0, 0, PACKET_REQUEST));
}

bool serial_controller::sendSerialFrame(packet_information_t frame)
{
    packet_t packet = encoderSingle(frame);
    // Send the packet in serial and wait the received data
    packet_t receive = sendSerialPacket(packet);
    return parse_packet(receive);
}

bool serial_controller::parse_packet(packet_t receive)
{
    if(receive.length > 0)
    {
        // Read all frame and if is true send a packet with all new information
        for (int i = 0; i < receive.length; i += receive.buffer[i]) {
            packet_information_t info;
            memcpy((unsigned char*) &info, &receive.buffer[i], receive.buffer[i]);
            if(info.type == 0)
            {
                ROS_DEBUG("Return alive message");
            }
            // Check if is available on the hashmap
            else if (hashmap.find(info.type) != hashmap.end())
            {
                // Send the message
                callback_data_packet_t callback = hashmap[info.type];
                callback(info.option, info.type, info.command, info.message);
            }
        }
        mStatus = SERIAL_OK;
        return true;
    }
    mStatus = SERIAL_EMPTY;
    return false;
}

bool serial_controller::sendSerialFrame(vector<packet_information_t> list_send)
{
    if(list_send.size())
    {
        // Encode the list of frames
        packet_t packet;
        unsigned int n_packet = encoder(&packet, list_send.data(), list_send.size());
        if(n_packet == list_send.size())
        {
            // Send the packet in serial and wait the received data
            packet_t receive = sendSerialPacket(packet);
            // Parse packet
            return parse_packet(receive);
        }
        ROS_ERROR_STREAM("Buffer FULL");
        mStatus = SERIAL_BUFFER_FULL;
    }
    return true;
}

packet_t serial_controller::sendSerialPacket(packet_t packet)
{
    if(mSerial.isOpen())
    {
        writePacket(packet);
        if(readPacket())
        {
            return mReceive;
        }
    }
    packet_t empty;
    empty.length = 0;
    return empty;
}

bool serial_controller::writePacket(packet_t packet)
{
    // Size of the packet
    int dataSize = (LNG_PACKET_HEADER + packet.length + 1);

    ROS_DEBUG_STREAM( "To be written " << dataSize << " bytes" );
    //Build a message to send to serial
    build_pkg(BufferTx, packet);
    // Send the packet on serial
    int written = 0;
    try
    {
        written = mSerial.write(BufferTx, dataSize);
    }
    catch (serial::SerialException& e)
    {
        mStatus = SERIAL_EXCEPTION;
        ROS_ERROR_STREAM("Unable to write serial port " << mSerialPort << " - Error: "  << e.what() );
        return false;
    }
    catch (serial::IOException& e)
    {
        mStatus = SERIAL_IOEXCEPTION;
        ROS_ERROR_STREAM("Unable to write serial port " << mSerialPort << " - Error: "  << e.what() );
        return false;
    }

    if ( written != dataSize )
    {
        ROS_WARN_STREAM( "Serial write error. Written " << written << " bytes instead of" << dataSize << " bytes.");
        return false;
    }
    return true;
}

bool serial_controller::readPacket()
{
    do {

        if( mStopping )
        {
            return false;
        }

        if( !mSerial.waitReadable() )
        {
            mStatus = SERIAL_TIMEOUT;
            ROS_ERROR_STREAM( "Serial timeout connecting");
            return false;
        }
        string reply;
        try
        {
            reply = mSerial.read( mSerial.available() );
        }
        catch (serial::SerialException& e)
        {
            mStatus = SERIAL_EXCEPTION;
            ROS_ERROR_STREAM("Unable to read serial port " << mSerialPort << " - Error: "  << e.what() );
            return false;
        }
        catch (serial::IOException& e)
        {
            mStatus = SERIAL_IOEXCEPTION;
            ROS_ERROR_STREAM("Unable to read serial port " << mSerialPort << " - Error: "  << e.what() );
            return false;
        }

        ROS_DEBUG_STREAM( "Received " << reply.size() << " bytes" );

        for (unsigned i=0; i<reply.length(); ++i)
        {
            unsigned char data = reply.at(i);
            if (decode_pkgs(data))
            {
                return true;
            }
        }
    } while(true);
}

}
