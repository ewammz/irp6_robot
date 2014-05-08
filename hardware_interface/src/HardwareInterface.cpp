#include <rtt/TaskContext.hpp>
#include <rtt/Port.hpp>
#include <rtt/Component.hpp>
#include <irp6_msgs/energy.h>
#include <Eigen/Dense>
#include <std_msgs/Float64.h>
#include "rtt_rosclock/rtt_rosclock.h"

#include "hi_moxa.h"

//IRP6

const int NUMBER_OF_DRIVES = 6;
const int16_t MAX_CURRENT[] = {25000, 18000, 15000, 17000, 10000, 2000};
const double MAX_INCREMENT[] = { 1000, 1000, 1000, 1000, 1000, 1000 };
const unsigned int CARD_ADDRESSES[] = { 0, 1, 2, 3, 4, 5 };
const int TX_PREFIX_LEN = 0;

const double GEAR[6] = {-158.0, 2*M_PI/5.0, 2*M_PI/5.0, -128.0, -128.0*0.6, 288.8845};
const double THETA[6] = {0.0, 2.203374e+02, 1.838348e+02, 1.570796e+00, 0.0, 0.0};

const double SYNCHRO_MOTOR_POSITION[6] = {-15.9, -5.0, -8.527, 151.31, 432.25, 791.0};
const double 	SYNCHRO_JOINT_POSITION[6] = { SYNCHRO_MOTOR_POSITION[0] - GEAR[0] * THETA[0],
                                            SYNCHRO_MOTOR_POSITION[1] - GEAR[1] * THETA[1],
                                              SYNCHRO_MOTOR_POSITION[2] - GEAR[2] * THETA[2],
                                              SYNCHRO_MOTOR_POSITION[3] - GEAR[3] * THETA[3],
                                              SYNCHRO_MOTOR_POSITION[4] - GEAR[4] * THETA[4] - SYNCHRO_MOTOR_POSITION[3],
                                              SYNCHRO_MOTOR_POSITION[5] - GEAR[5] * THETA[5] };

const int ENC_RES[6] = {4000, 4000, 4000, 4000, 4000, 2000};
const double SYNCHRO_STEP_COARSE[6] = {-0.03, -0.03, -0.03, -0.03, -0.03, -0.05};
const double SYNCHRO_STEP_FINE[6] = {0.007, 0.007, 0.007, 0.007, 0.007, 0.05};


using namespace RTT;

typedef enum { NOT_SYNCHRONIZED, SERVOING, SYNCHRONIZING } State;
typedef enum { MOVE_TO_SYNCHRO_AREA, STOP, MOVE_FROM_SYNCHRO_AREA, WAIT_FOR_IMPULSE, SYNCHRO_END } SynchroState;


class HardwareInterface : public RTT::TaskContext{

private: 
 
InputPort<std::vector<double> >computedPwm_in;

OutputPort<std::vector<double> >posInc_out;
OutputPort<std::vector<double> >energy_out;
OutputPort<irp6_msgs::energy>power_out;
OutputPort<std::vector<int> >deltaInc_out;

OutputPort<Eigen::VectorXd > port_motor_position_;
InputPort<Eigen::VectorXd > port_motor_position_command_;

Eigen::VectorXd motor_position_, motor_position_command_, motor_position_command_old_;

int number_of_drives;
bool auto_synchronize;

double counter;

State state;
SynchroState synchro_state;
int synchro_drive;

std::vector<double> pos_inc;
std::vector<double> energy;
std::vector<int> increment;
std::vector<int> current;
float total_power;
float sum_total_power;
std::vector<float> voltage;
std::vector<double> motor_pos;
std::vector<double> old_pwm;
std::vector<double> pwm;

irp6_msgs::energy msg;

hi_moxa::HI_moxa hi_;

bool debug;

public:

HardwareInterface(const std::string& name):
	TaskContext(name, PreOperational),
    hi_(NUMBER_OF_DRIVES-1, CARD_ADDRESSES, MAX_INCREMENT, TX_PREFIX_LEN)
{
    this->addPort("computedPwm_in", computedPwm_in).doc("Receiving a value of computed PWM.");
    this->addPort("posInc_out", posInc_out).doc("Sends out a value of expected position increment.");
    this->addPort("power_out", power_out).doc("Sends out a value of total power.");
    this->addPort("energy_out", energy_out).doc("Sends out a value of energy.");
    this->addPort("deltaInc_out", deltaInc_out).doc("Sends out a value increment increase in cycle.");

    this->ports()->addPort("MotorPosition", port_motor_position_);
    this->ports()->addPort("MotorPositionCommand", port_motor_position_command_);

    this->addProperty("number_of_drives", number_of_drives).doc("Number of drives in robot");
}

~HardwareInterface(){}

private:


bool configureHook()
{

  counter = 0.0;
  debug = true;
  auto_synchronize = true;

  increment.resize(number_of_drives);
  current.resize(number_of_drives);
  voltage.resize(number_of_drives);
  pos_inc.resize(number_of_drives);
  pwm.resize(number_of_drives);
  old_pwm.resize(number_of_drives);
  energy.resize(number_of_drives);

  for(int i=0; i<number_of_drives; i++)
  {
    increment[i] = 0;
    current[i] = 0;
    voltage[i] = 0;
    pos_inc[i] = 0;
    pwm[0] = 0;
    old_pwm[0] = 0;
    energy[i] = 0;
    msg.energy[i]=0;
  }

  std::vector<std::string> ports;

  //irp6

  ports.push_back("/dev/ttyMI8");
  ports.push_back("/dev/ttyMI9");
  ports.push_back("/dev/ttyMI10");
  ports.push_back("/dev/ttyMI11");
  ports.push_back("/dev/ttyMI12");
  ports.push_back("/dev/ttyMI13");


	try
	{
		hi_.init(ports);
    for(int i=0; i<number_of_drives; i++)
    {
      hi_.set_parameter_now(i, NF_COMMAND_SetDrivesMaxCurrent, MAX_CURRENT[i]);
      hi_.set_pwm_mode(i);
    }
	}
	catch (std::exception& e)
	{
		log(Info) << e.what() << endlog();
		return false;
	}

  motor_position_.resize(number_of_drives);
  motor_position_command_.resize(number_of_drives);
  motor_position_command_old_.resize(number_of_drives);
  
	return true;

}

bool startHook()
{
    try
    {

        hi_.HI_read_write_hardware();

        if(!hi_.robot_synchronized())
        {
            RTT::log(RTT::Info) << "Robot not synchronized" << RTT::endlog();
            if(auto_synchronize)
            {
                RTT::log(RTT::Info) << "Auto synchronize" << RTT::endlog();
                state = SYNCHRONIZING;
                synchro_state = MOVE_TO_SYNCHRO_AREA;
                synchro_drive = 0;
            }
            else
                state = NOT_SYNCHRONIZED;

        }
        else
        {
            RTT::log(RTT::Info) << "Robot synchronized" << RTT::endlog();

            for(int i=0; i<number_of_drives; i++)
            {
                motor_position_command_(i) = (double)hi_.get_position(i) * ((2.0 * M_PI) / ENC_RES[i]);
                motor_position_command_old_(i) = motor_position_command_(i);
            }

            state = SERVOING;
        }
    }
    catch (const std::exception& e)
    {
      RTT::log(RTT::Error) << e.what() << RTT::endlog();
      return false;
    }

    for(int i=0; i<number_of_drives; i++)
    {
        pos_inc[i] = 0.0;
    }
    
    return true;
}


void updateHook()
{


    if(NewData!=computedPwm_in.read(pwm)) {
        RTT::log(RTT::Error) << "NO PWM DATA" << RTT::endlog();
    }

    for(int i=0; i<number_of_drives; i++)
    {
    	old_pwm[i] = pwm[i];
    	hi_.set_pwm(i, pwm[i]);
    }

    hi_.HI_read_write_hardware();

    switch(state)
    {
        case NOT_SYNCHRONIZED :

            for(int i=0; i<number_of_drives; i++)
            {
                pos_inc [i]= 0.0;
            }
        break;

        case SERVOING :
            if (port_motor_position_command_.read(motor_position_command_) == RTT::NewData)
            {
                for(int i=0; i<number_of_drives; i++)
                {
                    pos_inc[i] =  (motor_position_command_(i) - motor_position_command_old_(i)) * (ENC_RES[i] / (2.0 * M_PI));
                    motor_position_command_old_(i) = motor_position_command_(i);
                }
            }
            else
            {
                for(int i=0; i<number_of_drives; i++)
                {
                    pos_inc[i] = 0.0;
                }
            }
            
            for(int i=0; i<number_of_drives; i++)
            {
              motor_position_(i) = (double)hi_.get_position(i) * ((2.0 * M_PI) / ENC_RES[i]);
            }
            port_motor_position_.write(motor_position_);
        break;

        case SYNCHRONIZING :
            switch(synchro_state)
            {
                case MOVE_TO_SYNCHRO_AREA :
                    if(hi_.in_synchro_area(synchro_drive))
                    {
                        RTT::log(RTT::Debug) << "[servo " << synchro_drive << " ] MOVE_TO_SYNCHRO_AREA ended" << RTT::endlog();
                        pos_inc[synchro_drive] = 0.0;
                        synchro_state = STOP;
                    }
                    else
                    {
                        //ruszam powoli w stronę synchro area
                        RTT::log(RTT::Debug) << "[servo " << synchro_drive << " ] MOVE_TO_SYNCHRO_AREA" << RTT::endlog();
                        pos_inc[synchro_drive] = SYNCHRO_STEP_COARSE[synchro_drive] * (ENC_RES[synchro_drive]/(2.0*M_PI));
                    }
                break;

                case STOP :
                    //tutaj jakis timeout
                    hi_.start_synchro(synchro_drive);
                    synchro_state = MOVE_FROM_SYNCHRO_AREA;

                break;

                case MOVE_FROM_SYNCHRO_AREA :
                    if(!hi_.in_synchro_area(synchro_drive))
                    {
                        RTT::log(RTT::Debug) << "[servo " << synchro_drive << " ] MOVE_FROM_SYNCHRO_AREA ended" << RTT::endlog();

                        synchro_state = WAIT_FOR_IMPULSE;
                    }
                    else
                    {
                        RTT::log(RTT::Debug) << "[servo " << synchro_drive << " ] MOVE_FROM_SYNCHRO_AREA" << RTT::endlog();
                        pos_inc[synchro_drive] = SYNCHRO_STEP_FINE[synchro_drive] * (ENC_RES[synchro_drive]/(2.0*M_PI));
                    }
                break;

                case WAIT_FOR_IMPULSE:
                    if(hi_.drive_synchronized(synchro_drive))
                    {
                        RTT::log(RTT::Debug)  << "[servo " << synchro_drive << " ] WAIT_FOR_IMPULSE ended" << RTT::endlog();

                        for(int i=0; i<number_of_drives; i++)
                        {
                            pos_inc [i]= 0.0;
                        }

                        hi_.finish_synchro(synchro_drive);
                        hi_.reset_position(synchro_drive);

                        motor_position_command_(synchro_drive) = (double)hi_.get_position(synchro_drive) * ((2.0 * M_PI) / ENC_RES[synchro_drive]);
                        motor_position_command_old_(synchro_drive) = motor_position_command_(synchro_drive);
                        if(++synchro_drive < number_of_drives)
                        {
                          synchro_state = MOVE_TO_SYNCHRO_AREA;
                        }
                        else
                        {
                          synchro_state = SYNCHRO_END;
                        }

                    }
                    else
                    {
                        RTT::log(RTT::Debug) << "[servo " << synchro_drive << " ] WAIT_FOR_IMPULSE" << RTT::endlog();
                        pos_inc[synchro_drive] = SYNCHRO_STEP_FINE[synchro_drive] * (ENC_RES[synchro_drive]/(2.0*M_PI));
                    }
                break;

                case SYNCHRO_END :

                    RTT::log(RTT::Debug) << "[servo " << synchro_drive << " ] SYNCHRONIZING ended" << RTT::endlog();
                    state = SERVOING;
                break;
            }
        break;
    }


    for(int i=0; i<number_of_drives; i++)
    {
        increment[i]= hi_.get_increment(i);

        if(abs(increment[i]) > 400)
        {
            increment[i] = 0;
        }

        if(fabs(pos_inc[i]) > 400)
        {
            pos_inc[i] = 0;
        }
    }

    if(state==SERVOING)
    {
    	total_power=0.0;
    	ros::Time now = rtt_rosclock::host_rt_now();
    	msg.header.stamp=now;
        for(int i=0; i<number_of_drives; i++)
        {
        	current[i] = hi_.get_current(i);
        	float axis_power = ((float) abs(current[i]))/1000.0 * fabs(old_pwm[i]/255.0)*hi_.get_voltage(i);
        	total_power += axis_power;
        }
        for(int i=0; i<number_of_drives; i++)
        {
        	float step_energy = ((float) abs(current[i])) / 1000.0 * fabs(old_pwm[i]/255.0) * (hi_.get_voltage(i) - (total_power / 40.0))* (0.002);
        	energy[i]=step_energy;
        	msg.energy[i] += energy[i];
        }
        msg.power = total_power;
        power_out.write(msg);
        energy_out.write(energy);
    }

    deltaInc_out.write(increment);
    posInc_out.write(pos_inc);
}

};
ORO_CREATE_COMPONENT(HardwareInterface)
