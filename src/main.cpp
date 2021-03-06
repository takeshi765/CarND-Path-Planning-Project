#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

double distance(double x1, double y1, double x2, double y2)
{
	return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}
int ClosestWaypoint(double x, double y, const vector<double> &maps_x, const vector<double> &maps_y)
{

	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for(int i = 0; i < maps_x.size(); i++)
	{
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x,y,map_x,map_y);
		if(dist < closestLen)
		{
			closestLen = dist;
			closestWaypoint = i;
		}

	}

	return closestWaypoint;

}

int NextWaypoint(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{

	int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2( (map_y-y),(map_x-x) );

	double angle = abs(theta-heading);

	if(angle > pi()/4)
	{
		closestWaypoint++;
	}

	return closestWaypoint;

}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

	int prev_wp;
	prev_wp = next_wp-1;
	if(next_wp == 0)
	{
		prev_wp  = maps_x.size()-1;
	}

	double n_x = maps_x[next_wp]-maps_x[prev_wp];
	double n_y = maps_y[next_wp]-maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
	double proj_x = proj_norm*n_x;
	double proj_y = proj_norm*n_y;

	double frenet_d = distance(x_x,x_y,proj_x,proj_y);

	//see if d value is positive or negative by comparing it to a center point

	double center_x = 1000-maps_x[prev_wp];
	double center_y = 2000-maps_y[prev_wp];
	double centerToPos = distance(center_x,center_y,x_x,x_y);
	double centerToRef = distance(center_x,center_y,proj_x,proj_y);

	if(centerToPos <= centerToRef)
	{
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for(int i = 0; i < prev_wp; i++)
	{
		frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
	}

	frenet_s += distance(0,0,proj_x,proj_y);

	return {frenet_s,frenet_d};

}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, const vector<double> &maps_s, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int prev_wp = -1;

	while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) ))
	{
		prev_wp++;
	}

	int wp2 = (prev_wp+1)%maps_x.size();

	double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s-maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
	double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

	double perp_heading = heading-pi()/2;

	double x = seg_x + d*cos(perp_heading);
	double y = seg_y + d*sin(perp_heading);

	return {x,y};

}

//I referenced the class material for vehicle class, position_at and generate_predictions.
class Vehicle {
public:
	int lane;
	double s;
	double v;
	double a;
	Vehicle();
    Vehicle(int lane, double s, double v, double a){
    	this->lane = lane;
    	this->s = s;
    	this->v = v;
    	this->a = a;
    };
};

double position_at(double s, double v, double a, int t) {
    return s + v*t + a*t*t/2.0;
}

vector<Vehicle> generate_predictions(int lane, double s, double v, double a, int horizon){
		vector<Vehicle> predictions;
	    for(int i = 0; i < horizon; i++) {
	    	double next_s = position_at(s, v, a, i);
	    	double next_v = 0;
	      if (i < horizon-1) {
	        next_v = position_at(s, v, a, i+1) - s;
	      }
	      predictions.push_back(Vehicle(lane, next_s, next_v, 0.0));
	  	}
	    return predictions;
}



int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
  	istringstream iss(line);
  	double x;
  	double y;
  	float s;
  	float d_x;
  	float d_y;
  	iss >> x;
  	iss >> y;
  	iss >> s;
  	iss >> d_x;
  	iss >> d_y;
  	map_waypoints_x.push_back(x);
  	map_waypoints_y.push_back(y);
  	map_waypoints_s.push_back(s);
  	map_waypoints_dx.push_back(d_x);
  	map_waypoints_dy.push_back(d_y);
  }

  /*
   * I referenced the project walkthrough video for path planning.
   */
  //start lane 1
  int lane = 1; //you need to add this parameter to lambda (h.onMessage ...)
  int lane_change_count = 0;
  //Have a reference velocity to target
  double ref_vel = 0.0; //mph //you need to add this parameter to lambda (h.onMessage ...)

  h.onMessage([&lane_change_count, &ref_vel, &lane, &map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    //auto sdata = string(data).substr(0, length);
    //cout << sdata << endl;
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);
        
        string event = j[0].get<string>();
        
        if (event == "telemetry") {
            // j[1] is the data JSON object
          
        	  // Main car's localization Data
          	double car_x = j[1]["x"];
          	double car_y = j[1]["y"];
          	double car_s = j[1]["s"];
          	double car_d = j[1]["d"];
          	double car_yaw = j[1]["yaw"];
          	double car_speed = j[1]["speed"];

          	// Previous path data given to the Planner
          	auto previous_path_x = j[1]["previous_path_x"];
          	auto previous_path_y = j[1]["previous_path_y"];
          	// Previous path's end s and d values 
          	double end_path_s = j[1]["end_path_s"];
          	double end_path_d = j[1]["end_path_d"];

          	// Sensor Fusion Data, a list of all other cars on the same side of the road.
          	auto sensor_fusion = j[1]["sensor_fusion"];

          	json msgJson;

          	//Define the actual (x,y) poitns we will use for the planner
          	vector<double> next_x_vals;
          	vector<double> next_y_vals;

          	int prev_size = previous_path_x.size();

          	if(prev_size > 0)
          	{
          		car_s = end_path_s;
          	}

          	double speed_limit_of_this_car = 49.5;
          	double accerelation_value = 0.224; // 5m/s
          	bool too_close = false;
          	vector<bool> too_close_lane = {false,false,false}; //distance (s in Frenet) from other cars are closer than some threshold.
          	double too_close_threshold = 30.0;
          	double speed_lover_threshold = 50.0;
          	//find ref_v to use
          	//TODO: consider cost function
          	double min_too_close = 100;
          	double velocity_of_front_car = 0;
            for(int i = 0; i < sensor_fusion.size(); ++i)
            {
              //check i_th car is in my lane
              float d = sensor_fusion[i][6]; //i th car   6:d value
              double vx = sensor_fusion[i][3];
              double vy = sensor_fusion[i][4];
              double check_speed = sqrt(vx*vx+vy*vy); //magnitude of speed
              double check_car_s = sensor_fusion[i][5]; // 5: s value of the car

              check_car_s+=((double)prev_size*0.02*check_speed);//if using previous points can project s value out, future car position?
              //check s values greater thatn mine and s gap
              double dist_cars = check_car_s-car_s;


              //check the distance(in s) to cars in all lanes. This is used to for safety check.
              for(int temp_lane =0;temp_lane<3;temp_lane++){
                if(d < (2+4*temp_lane+2) && d> (2+4*temp_lane-2))
                {
                  if((check_car_s>=car_s)&&((dist_cars)<30.0))
                  {
                    //This condition checks whether this car is in the same lane as my car.
                    if(d < (2+4*lane+2) && d> (2+4*lane-2)){
                      too_close=true;
                      if(min_too_close>dist_cars){
                        min_too_close = dist_cars;
                        velocity_of_front_car = check_speed;
                      }

                    }

                  }
                  if(fabs(check_car_s-car_s)<15){
                    too_close_lane[temp_lane]=true;
                  }
                  // additional step
                  double temp_acceleration = 0.0;
                  int horizon = 3; //it considers 5 seconds in the future
                  vector<Vehicle> predictions_other_car = generate_predictions(temp_lane, check_car_s, check_speed, temp_acceleration, horizon);
                  double accleration_of_this_car = 0.0;
                  if(too_close){
                    accleration_of_this_car = -accerelation_value;
                  }else{
                    if(ref_vel < speed_limit_of_this_car){
                      accleration_of_this_car = accerelation_value;
                    }
                  }
                  vector<Vehicle> predictions_this_car  = generate_predictions(temp_lane, car_s, car_speed, accerelation_value, horizon);

                  for(int ci=0;ci<predictions_this_car.size();++ci){

                    //If any car in the final lane is 15.0m within the car position, it is considerd as too close.
                    if(fabs(predictions_other_car[ci].s-predictions_this_car[ci].s)<15.0){
//                      cout << ci << " future_too_close " << endl;
                      too_close_lane[temp_lane]=true;
                    }

                  }

                }
              }

          }
      		cout << "too close lane: ";
      		for(size_t vi=0;vi<too_close_lane.size();vi++){
      			cout << too_close_lane[vi] << ",";
      		}
      		cout << endl;

      		string strategy="simple_distance_check";

          if(too_close)
          {
            //if the front car is too close, reduce reference velocity
            ref_vel -= accerelation_value; // 5 m/s
            //if the distance between the car and the front car is less than 7m, reduce reference velocity more
            if(min_too_close<7){
              cout <<"too close less than 7m" << endl;
              ref_vel -= accerelation_value;
              ref_vel -= accerelation_value;
            }
          		//TODO
            if (strategy=="simple_distance_check"){
          			// This strategy takes about 5 min 30 seconds.
              lane_change_count+=1;
              if((lane==0)&&(too_close_lane[1]==false)){
                lane = 1;
                cout << lane_change_count << " current lane 0 shift to 1" << endl;
              }else if((lane==1)&&(too_close_lane[0]==false)){
                lane = 0;
                cout <<lane_change_count << " current lane 1 shift to 2" << endl;
              }else if((lane==1)&&(too_close_lane[2]==false)){
                lane = 2;
                cout <<lane_change_count << " current lane 1 shift to 2" << endl;
              }else if((lane==2)&&(too_close_lane[1]==false)){
                lane = 1;
                cout <<lane_change_count << " current lane 2 shift to 1" << endl;
              }
            }else{

            }

          }
          else if(ref_vel < speed_limit_of_this_car)
          {
            ref_vel += accerelation_value;
          }

          //create a list of widely spaced (x,y) waypoints, evenly spaced at 30m
          //Later we will interpolate these waypoints with a spline and fill it in with more points that control speed
          vector<double> ptsx;
          vector<double> ptsy;

          //reference x,y,yaw states
          // either we will reference the starting point as where the car is or at the previous paths end point
          double ref_x = car_x;
          double ref_y = car_y;
          double ref_yaw = deg2rad(car_yaw);

          //if previous size is almost empty, use the car as starting reference
          if(prev_size < 2)
          {
            //use tow points that make the path tangent to the car
            double prev_car_x = car_x - cos(car_yaw);
            double prev_car_y = car_y - sin(car_yaw);

            ptsx.push_back(prev_car_x);
            ptsx.push_back(car_x);

            ptsy.push_back(prev_car_y);
            ptsy.push_back(car_y);
          }
          //use the previous path's end point as starting reference
          else
          {
            //Redefine reference state as previous path end point
            ref_x = previous_path_x[prev_size-1];
            ref_y = previous_path_y[prev_size-1];

            double ref_x_prev = previous_path_x[prev_size-2];
            double ref_y_prev = previous_path_y[prev_size-2];
            ref_yaw = atan2(ref_y-ref_y_prev, ref_x-ref_x_prev);

            //use tow points that make the path tangent to the previous path's end point
            ptsx.push_back(ref_x_prev);
            ptsx.push_back(ref_x);

            ptsy.push_back(ref_y_prev);
            ptsy.push_back(ref_y);
          }

          //In Frenet add evenly 30m spaced points ahead of the starting reference
          vector<double> next_wp0 = getXY(car_s+30,(2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector<double> next_wp1 = getXY(car_s+60,(2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector<double> next_wp2 = getXY(car_s+90,(2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);


      		ptsx.push_back(next_wp0[0]);
      		ptsx.push_back(next_wp1[0]);
      		ptsx.push_back(next_wp2[0]);

      		ptsy.push_back(next_wp0[1]);
      		ptsy.push_back(next_wp1[1]);
      		ptsy.push_back(next_wp2[1]);

      		for(int i=0; i< ptsx.size(); i++)
      		{
      			//shift car reference angle to 0 degrees
      			double shift_x = ptsx[i] - ref_x;
      			double shift_y = ptsy[i] - ref_y;

      			ptsx[i] = (shift_x * cos(0-ref_yaw)-shift_y*sin(0-ref_yaw));
      			ptsy[i] = (shift_x * sin(0-ref_yaw)+shift_y*cos(0-ref_yaw));
      		}

      		// create a spline
      		tk::spline s;
      		s.set_points(ptsx,ptsy); //ex 5 anchor points


      		//start with all of the previous path points from last time
      		//This helps transition.
      		for(int i = 0;i < previous_path_x.size(); ++i){
      			next_x_vals.push_back(previous_path_x[i]);
      			next_y_vals.push_back(previous_path_y[i]);
      		}
      		// calculate how to break up spline points so that we travel at our desired reference velocity
      		double target_x = 30.0;
      		double target_y = s(target_x);
//      		double target_dist = sqrt(pow(target_x,2)+pow(target_y,2)); //N*0.02*velocity = distance
      		double target_dist = sqrt(target_x*target_x+target_y*target_y); //N*0.02*velocity = distance

      		double x_add_on = 0;
      		// Fill up the rest of our path planner after filling it with previous points, here we will always output 50 points
      		for(int i=1; i<=50-previous_path_x.size();i++)
      		{
      			double N = (target_dist/(0.02*ref_vel/2.24));
      			double x_point = x_add_on + (target_x)/N;
      			double y_point = s(x_point);

      			x_add_on = x_point;

      			double x_ref = x_point;
      			double y_ref = y_point;

      			//rotate back to normal after rotateing it earlier
      			x_point = (x_ref*cos(ref_yaw)-y_ref*sin(ref_yaw));
      			y_point = (x_ref*sin(ref_yaw)+y_ref*cos(ref_yaw));

      			x_point += ref_x;
      			y_point += ref_y;

      			next_x_vals.push_back(x_point);
      			next_y_vals.push_back(y_point);

      		}
          	//==========================================================================================
          	msgJson["next_x"] = next_x_vals;
          	msgJson["next_y"] = next_y_vals;


          	auto msg = "42[\"control\","+ msgJson.dump()+"]";

          	//this_thread::sleep_for(chrono::milliseconds(1000));
          	ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
          
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
