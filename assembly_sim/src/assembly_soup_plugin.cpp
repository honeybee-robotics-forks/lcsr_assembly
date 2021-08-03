#include <boost/bind.hpp>
#include <gazebo/gazebo.hh>
#include <gazebo/physics/physics.hh>
#include <gazebo/common/common.hh>
#include <stdio.h>

#include <boost/make_shared.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

// tf headers
#include <tf/transform_broadcaster.h>
#include <tf_conversions/tf_kdl.h>

// visualize mates in rviz gui
#include <visualization_msgs/MarkerArray.h>

// send information to ros
#include <assembly_msgs/Mate.h>
#include <assembly_msgs/MateList.h>
#include <assembly_msgs/MatingList.h>

#include <kdl/frames_io.hpp>

#include "assembly_soup_plugin.h"
#include "models.h"
#include "util.h"

/************************************************************************************/
/*                                Assembly Sim                                      */
/************************************************************************************/

namespace assembly_sim
{

  AssemblySoup::AssemblySoup() :
    mate_id_counter_(0),
    atom_id_counter_(0),
    tf_world_frame_("world"),
    broadcast_tf_(false),
    publish_active_mates_(false),
    last_tick_(0),
    updates_per_second_(10),
    running_(false)
  {
  }

  bool AssemblySoup::SuppressMatesCallback(assembly_msgs::SetMateSuppress::Request& req, assembly_msgs::SetMateSuppress::Response& res)
  {
    // Iterate over all mates
    unsigned int iter = 0;
    for (boost::unordered_set<MatePtr>::iterator it = mates_.begin();
         it != mates_.end();
         ++it, ++iter)
    {
      MatePtr mate = *it;
      std::string desc = mate->getDescription();

      std::string male_name = mate->male->link->GetName();
      std::string female_name = mate->female->link->GetName();

      gzerr << "Mate desc: " << desc << std::endl;

      // Assume a mate is not suppressed unless the contents of this
      // message declare otherwise
      mate->suppressMate(false);

      // Look for a mate that matches the description
      if ((male_name.compare(req.link_name_a) == 0 && female_name.compare(req.link_name_b) == 0) |
          (male_name.compare(req.link_name_b) == 0 && female_name.compare(req.link_name_a) == 0))
      {
        if(req.suppress)
        {
          gzwarn << "Suppress Mate - found matching mate for: " << desc << std::endl;
          mate->suppressMate(true);
          res.suppressed = true;
          return true;
        }
        else
        {
          gzwarn << "Unsuppress Mate - found matching mate for: " << desc << std::endl;
          mate->suppressMate(false);
          res.suppressed = false;
          return true;
        }
      }
    }

    res.suppressed = false;
    return false;
  }

  void AssemblySoup::Load(gazebo::physics::ModelPtr _parent, sdf::ElementPtr _sdf)
  {
    // Store the pointer to the model
    this->model_ = _parent;
    this->sdf_ = _sdf;

    // Create a node handle for ros topics
    ros::NodeHandle nh;
    suppress_mates_srv_ = nh.advertiseService("suppress_mates", &AssemblySoup::SuppressMatesCallback, this);

    // Subscribe to the suppress mates topic

    // Get TF configuration
    sdf::ElementPtr broadcast_elem = _sdf->GetElement("tf_world_frame");
    if (broadcast_elem) {
      broadcast_elem->GetValue()->Get(tf_world_frame_);
      gzwarn<<"Broadcasting TF frames for joints relative to \""<<tf_world_frame_<<"\""<<std::endl;
      broadcast_tf_ = true;

      // set up publishers for visualization
      male_mate_pub_ = nh.advertise<visualization_msgs::MarkerArray>("male_mate_points",1000);
      female_mate_pub_ = nh.advertise<visualization_msgs::MarkerArray>("female_mate_points",1000);
      wrenches_pub_ = nh.advertise<visualization_msgs::MarkerArray>("mate_wrenches",100);
      mate_details_pub_ = nh.advertise<visualization_msgs::MarkerArray>("mate_details",100);
    }

    // are we going to publish ros messages describing mate status?
    if(_sdf->HasElement("publish_active_mates")) {
      sdf::ElementPtr publish_mate_elem = _sdf->GetElement("publish_active_mates");
      publish_mate_elem->GetValue()->Get(publish_active_mates_);
      if (publish_active_mates_) {
        ros::NodeHandle nh;
        active_mates_pub_ = nh.advertise<assembly_msgs::MateList>("active_mates",1000);
        mating_pub_ = nh.advertise<assembly_msgs::MatingList>("mating_mates",1000);
        gzwarn << "Publishing active mates!" << std::endl;
      } else {
        gzwarn << "Not publishing active mates!" << std::endl;
      }
    } else {
      gzwarn<<"No \"publish_active_mates\" element."<<std::endl;
    }

    if(_sdf->HasElement("updates_per_second")) {
      sdf::ElementPtr updates_per_second_elem = _sdf->GetElement("updates_per_second");
      updates_per_second_elem->GetValue()->Get(updates_per_second_);
    }

    //gzwarn<<"Getting mate types..."<<std::endl;
    // Get the description of the mates in this soup
    sdf::ElementPtr mate_elem = _sdf->GetElement("mate_model");

    while(mate_elem && mate_elem->GetName() == "mate_model")
    {
      // Create a new mate model
      std::string model;
      if(mate_elem->HasAttribute("model")) {
        mate_elem->GetAttribute("model")->Get(model);
      } else {
        gzerr<<"ERROR: no mate model type for mate model"<<std::endl;
        return;
      }

      // Get the model name
      std::string mate_model_type;
      if(mate_elem->HasAttribute("type")) {
        mate_elem->GetAttribute("type")->Get(mate_model_type);
      } else {
        gzerr<<"ERROR: no mate type for mate model"<<std::endl;
        return;
      }

      if(mate_models_.find(mate_model_type) == mate_models_.end()) {
        // Determine the type of mate model
        gzlog<<"Adding mate model for "<<mate_model_type<<std::endl;

        // Store this mate model
        MateModelPtr mate_model = std::make_shared<MateModel>(mate_model_type, mate_elem);
        mate_models_[mate_model->type] = mate_model;

        // Create a mate factory
        MateFactoryBasePtr mate_factory;
        if(model == "proximity") {
          mate_factory = std::make_shared<MateFactory<ProximityMate> >(mate_model, model_);
        } else if(model == "dipole") {
          mate_factory = std::make_shared<MateFactory<DipoleMate> >(mate_model, model_);
        } else {
          gzerr<<"ERROR: \""<<model<<"\" is not a valid model type"<<std::endl;
          return;
        }
        mate_factories_[mate_model->type] = mate_factory;
      }

      // Get the next atom element
      mate_elem = mate_elem->GetNextElement(mate_elem->GetName());
    }

    //gzwarn<<"Getting atom models..."<<std::endl;
    // Get the description of the atoms in this soup
    sdf::ElementPtr atom_elem = _sdf->GetElement("atom_model");

    while(atom_elem && atom_elem->GetName() == "atom_model")
    {
      // Create a new atom
      AtomModelPtr atom_model = std::make_shared<AtomModel>();
      atom_elem->GetAttribute("type")->Get(atom_model->type);

      // Get the atom mate points
      sdf::ElementPtr mate_elem = atom_elem->GetElement("mate_point");
      while(mate_elem)
      {
        std::string type;
        std::string gender;
        KDL::Frame base_pose;

        mate_elem->GetAttribute("type")->Get(type);
        mate_elem->GetAttribute("gender")->Get(gender);
        to_kdl(mate_elem->GetElement("pose"), base_pose);

        //gzwarn<<"Adding mate point type: "<<type<<" gender: "<<gender<<" at: "<<base_pose<<std::endl;

        MateModelPtr mate_model = mate_models_[type];

        if(not mate_model) {
          gzerr<<"No mate model for type: "<<type<<std::endl;
          break;
        }

        MatePointPtr mate_point;

        if(boost::iequals(gender, "female")) {
#if 0
          for(std::vector<KDL::Frame>::iterator pose_it = mate_model->symmetries.begin();
              pose_it != mate_model->symmetries.end();
              ++pose_it)
          {
            mate_point = std::make_shared<MatePoint>();
            mate_point->model = mate_model;
            mate_point->pose = base_pose * (*pose_it);
            mate_point->id =
              atom_model->female_mate_points.size()
              + atom_model->male_mate_points.size();

            gzwarn<<"Adding female mate point "<<atom_model->type<<"#"<<mate_point->id<<" pose: "<<std::endl<<mate_point->pose<<std::endl;

            atom_model->female_mate_points.push_back(mate_point);
          }
#else
          mate_point = std::make_shared<MatePoint>();
          mate_point->model = mate_model;
          mate_point->pose = base_pose;
          mate_point->id =
            atom_model->female_mate_points.size()
            + atom_model->male_mate_points.size();

          //gzwarn<<"Adding female mate point "<<atom_model->type<<"#"<<mate_point->id<<" pose: "<<std::endl<<mate_point->pose<<std::endl;

          atom_model->female_mate_points.push_back(mate_point);
#endif
        } else if(boost::iequals(gender, "male")) {
          mate_point = std::make_shared<MatePoint>();
          mate_point->model = mate_model;
          mate_point->pose = base_pose;
          mate_point->id =
            atom_model->female_mate_points.size()
            + atom_model->male_mate_points.size();

          //gzwarn<<"Adding male mate point "<<atom_model->type<<"#"<<mate_point->id<<" pose: "<<std::endl<<mate_point->pose<<std::endl;

          atom_model->male_mate_points.push_back(mate_point);
        } else {
          gzerr<<"Unknown gender: "<<gender<<std::endl;
        }

        // Get the next mate point element
        mate_elem = mate_elem->GetNextElement(mate_elem->GetName());
      }

      // Store this atom
      atom_models_[atom_model->type] = atom_model;

      // Get the next atom element
      atom_elem = atom_elem->GetNextElement(atom_elem->GetName());
    }

    //gzwarn<<"Extracting links..."<<std::endl;
    // Extract the links from the model
    std::vector<gazebo::physics::LinkPtr> assembly_links;

    std::queue<gazebo::physics::ModelPtr> models;
    models.push(this->model_);
    while(models.size() > 0) {
        gazebo::physics::ModelPtr m = models.front();
        models.pop();

        for(auto &l : m->GetLinks()) {
            assembly_links.push_back(l);
        }

        for(auto &nested_model : m->NestedModels()) {
            models.push(nested_model);
        }
    }

    // Create atoms for each link
    for(auto &link : assembly_links)
    {
      //gzwarn<<"Creating atom for link: "<<link->GetName()<<std::endl;

      // Create new atom
      AtomPtr atom = std::make_shared<Atom>();
      atom->link = link;

      // Determine the atom type from the link name
      for(std::map<std::string, AtomModelPtr>::iterator model_it=atom_models_.begin();
          model_it != atom_models_.end();
          ++model_it)
      {
        if(atom->link->GetName().find(model_it->second->type) == 0) {
          atom->model = model_it->second;
          break;
        }
      }

      // Skip this atom if it doesn't have a model
      if(not atom->model) {
        gzerr<<"Atom doesn't have a model type!"<<std::endl;
        continue;
      }

      //gzwarn<<"Atom "<<atom->link->GetName()<<" is a "<<atom->model->type<<std::endl;

      atoms_.push_back(atom);
    }

    // Iterate over all atoms and create potential mate objects
    for(std::vector<AtomPtr>::iterator it_fa = atoms_.begin();
        it_fa != atoms_.end();
        ++it_fa)
    {
      AtomPtr female_atom = *it_fa;
      //gzwarn<<"Inspecting female atom: "<<female_atom->link->GetName()<<std::endl;

      // Get the link associated with this atom
      // If any male mates match this link, ignore them
      // they are mate points on the same collection of links/joints
      gazebo::physics::Link_V parents;
      parents = female_atom->link->GetParentJointsLinks();

      // Iterate over all female mate points of female link
      for(std::vector<MatePointPtr>::iterator it_fmp = female_atom->model->female_mate_points.begin();
          it_fmp != female_atom->model->female_mate_points.end();
          ++it_fmp)
      {
        MatePointPtr female_mate_point = *it_fmp;

        // Iterate over all other atoms
        for(std::vector<AtomPtr>::iterator it_ma = atoms_.begin();
            it_ma != atoms_.end();
            ++it_ma)
        {
          AtomPtr male_atom = *it_ma;

          // You can't mate with yourself
          if(male_atom == female_atom) { continue; }

          // Don't mate if the male atom is on a link that matches
          // the female atom's link
          if (parents.size()!=0)
          {
            if (parents[0] == male_atom->link)
              {
                //gzwarn << "match found, not making joint " << std::endl;
                //gzwarn << "male link: " << male_atom->link <<std::endl;
                //gzwarn << "parent link: " << parents[0] <<std::endl;
                continue;
              }
          }

          // Iterate over all male mate points of male link
          for(std::vector<MatePointPtr>::iterator it_mmp = male_atom->model->male_mate_points.begin();
              it_mmp != male_atom->model->male_mate_points.end();
              ++it_mmp)
          {
            MatePointPtr male_mate_point = *it_mmp;

            // Skip if the mates are incompatible
            if(female_mate_point->model != male_mate_point->model) { continue; }

            // Construct the mate between these two mate points
            MatePtr mate = mate_factories_[female_mate_point->model->type]->createMate(
              female_mate_point,
              male_mate_point,
              female_atom,
              male_atom);

            mates_.insert(mate);
          }
        }
      }
    }

    // Listen to the update event. This event is broadcast every
    // simulation iteration.
    this->updateConnection_ = gazebo::event::Events::ConnectWorldUpdateBegin(
        boost::bind(&AssemblySoup::OnUpdate, this, _1));
  }

  void AssemblySoup::queueStateUpdates() {

    static tf::TransformBroadcaster br;

    assembly_msgs::MateList mates_msg;
    assembly_msgs::MatingList mating_msg;

    // Synchronize with main update thread
    boost::mutex::scoped_lock update_lock(update_mutex_);

    unsigned int iter = 0;

    // Iterate over all mates
    for (boost::unordered_set<MatePtr>::iterator it = mates_.begin();
         it != mates_.end();
         ++it, ++iter)
    {
      MatePtr mate = *it;

      assembly_msgs::Mate mate_msg;
      mate_msg.description = mate->getDescription();
      mate_msg.linear_error = mate->mate_point_error.vel.Norm();
      mate_msg.angular_error =mate->mate_point_error.rot.Norm();

      if(publish_active_mates_ and mate->state == Mate::MATED) {
        mates_msg.female.push_back(mate->joint->GetParent()->GetName());
        mates_msg.male.push_back(mate->joint->GetChild()->GetName());
        mates_msg.linear_error.push_back(mate_msg.linear_error);
        mates_msg.angular_error.push_back(mate_msg.angular_error);

        geometry_msgs::Quaternion symmetry_orientation;
        tf::quaternionKDLToMsg((*(mate->mated_symmetry)).M, symmetry_orientation);
        mates_msg.symmetry.push_back(symmetry_orientation);

      } else if(publish_active_mates_ and mate_msg.linear_error < 0.03 and mate_msg.linear_error > 0.0) {
        mating_msg.mates.push_back(mate_msg);
      }

#if 1
// TODO: confirm that this step is now uncessary as the update is handled below on line 434
//       checking for this update but not queing it can actually skip the update logic in the model
//
//      // Check if this mate is already scheduled to be updated
//      if(mate->needsUpdate()) {
//        //gzwarn<<"mate "<<mate->getDescription()<<" already scheduled."<<std::endl;
//        continue;
//      }

      // Queue any updates
      mate->queueUpdate();

      // Schedule mates to detach / attach etc
      if(mate->needsUpdate()) {
        //gzwarn<<"mate /"<<mate->getDescription()<<" needs to be updated"<<std::endl;
        mate_update_queue_.push(mate);
      }
#endif

#if 0
      // Broadcast the TF frame for this joint
      // TODO: move this introspection out of this thread
      if (broadcast_tf_ and mate->joint->GetParent() and mate->joint->GetChild())
      {
        tf::Transform tf_joint_frame;
        //to_kdl(male_atom->link->WorldPose() * mate->joint->InitialAnchorPose(), tf_frame);
        //to_tf(mate->joint->WorldPose(), tf_frame);

        ignition::math::Vector3d anchor = mate->joint->Anchor(0);

        KDL::Frame male_atom_frame;
        to_kdl(mate->male->link->WorldPose(), male_atom_frame);
        KDL::Frame male_mate_frame = male_atom_frame * mate->male_mate_point->pose * mate->anchor_offset;
        KDL::Frame joint_frame = KDL::Frame(
            male_mate_frame.M,
            KDL::Vector(anchor.X(), anchor.Y(), anchor.Z()));
        tf::poseKDLToTF(joint_frame, tf_joint_frame);

        br.sendTransform(
            tf::StampedTransform(
                tf_joint_frame,
                ros::Time::now(),
                tf_world_frame_,
                mate->joint->GetName()));
      }
#endif
    }

    // Broadcast TF frames for this mate
    // TODO: move this introspection out of this thread
    if(broadcast_tf_)
    {
      visualization_msgs::MarkerArray male_mate_markers;
      visualization_msgs::MarkerArray female_mate_markers;
      visualization_msgs::MarkerArray wrench_markers;
      visualization_msgs::MarkerArray mate_details_markers;

      unsigned int atom_id = 0;
      for(std::vector<AtomPtr>::iterator it_fa = atoms_.begin();
          it_fa != atoms_.end();
          ++it_fa,++atom_id)
      {
        AtomPtr female_atom = *it_fa;
        //gzwarn<<"broadcasting tf/marker info"<<std::endl;
        
        // Get the female atom frame
        KDL::Frame female_atom_frame;
        to_kdl(female_atom->link->WorldPose(), female_atom_frame);

        for (boost::unordered_set<MatePtr>::iterator it = mates_.begin();
             it != mates_.end();
             ++it)
        {
          MatePtr mate = *it;
          mate->getMarkers(mate_details_markers);
        }

        // Construct wrench arrow marker
        visualization_msgs::Marker wrench_marker;
        wrench_marker.ns = "wrenches";
        wrench_marker.id = atom_id;
        wrench_marker.header.frame_id = "/world";
        wrench_marker.header.stamp = ros::Time::now();
        wrench_marker.type = visualization_msgs::Marker::ARROW;
        wrench_marker.scale.x = 0.005;
        wrench_marker.scale.y = 0.01;
        wrench_marker.scale.z = 0.01;
        wrench_marker.color.a = 0.5;
        wrench_marker.color.r = 1.0;
        geometry_msgs::Point p0, p1;
        p0.x = female_atom_frame.p.x();
        p0.y = female_atom_frame.p.y();
        p0.z = female_atom_frame.p.z();
        p1.x = female_atom_frame.p.x() + female_atom->wrench.force.x();
        p1.y = female_atom_frame.p.y() + female_atom->wrench.force.y();
        p1.z = female_atom_frame.p.z() + female_atom->wrench.force.z();
        wrench_marker.points.push_back(p0);
        wrench_marker.points.push_back(p1);
        wrench_markers.markers.push_back(wrench_marker);

        // Construct some names for use with TF
        const std::string atom_name = boost::str(
            boost::format("%s")
            % female_atom->link->GetName());
        const std::string link_name = boost::str(
            boost::format("%s/%s")
            % atom_name
            % female_atom->model->type);

        tf::Transform tf_frame;

        // Broadcast a tf frame for this link
        to_tf(female_atom->link->WorldPose(), tf_frame);
        br.sendTransform(
            tf::StampedTransform(
                tf_frame,
                ros::Time::now(),
                tf_world_frame_,
                link_name));

        // Broadcast all male mate points for this atom
        for(std::vector<MatePointPtr>::iterator it_mmp = female_atom->model->male_mate_points.begin();
            it_mmp != female_atom->model->male_mate_points.end();
            ++it_mmp)
        {
          MatePointPtr male_mate_point = *it_mmp;

          const std::string male_mate_point_name = boost::str(
              boost::format("%s/male_%d")
              % atom_name
              % male_mate_point->id);

          tf::poseKDLToTF(male_mate_point->pose,tf_frame);
          br.sendTransform(
              tf::StampedTransform(
                  tf_frame,
                  ros::Time::now(),
                  link_name,
                  male_mate_point_name));

          visualization_msgs::Marker mate_marker;
          mate_marker.header.frame_id = male_mate_point_name;
          mate_marker.header.stamp = ros::Time(0);
          mate_marker.type = mate_marker.CUBE;
          mate_marker.action = mate_marker.ADD;
          mate_marker.id = (atom_id * 10000) + (iter * 100) + male_mate_point->id;
          mate_marker.scale.x = 0.02;
          mate_marker.scale.y = 0.02;
          mate_marker.scale.z = 0.01;
          mate_marker.color.r = 1.0;
          mate_marker.color.g = 0.0;
          mate_marker.color.b = 0.0;
          mate_marker.color.a = 0.25;
          male_mate_markers.markers.push_back(mate_marker);
        }

        // Broadcast all female mate points for this atom
        for(std::vector<MatePointPtr>::iterator it_fmp = female_atom->model->female_mate_points.begin();
            it_fmp != female_atom->model->female_mate_points.end();
            ++it_fmp)
        {
          MatePointPtr female_mate_point = *it_fmp;

          const std::string female_mate_point_name = boost::str(
              boost::format("%s/female_%d")
              % atom_name
              % female_mate_point->id);

          tf::poseKDLToTF(female_mate_point->pose, tf_frame);
          br.sendTransform(
              tf::StampedTransform(
                  tf_frame,
                  ros::Time::now(),
                  link_name,
                  female_mate_point_name));

          visualization_msgs::Marker mate_marker;
          mate_marker.header.frame_id = female_mate_point_name;
          mate_marker.header.stamp = ros::Time(0);
          mate_marker.type = mate_marker.CUBE;
          mate_marker.action = mate_marker.ADD;
          mate_marker.id = (atom_id * 10000) + (iter * 100) + female_mate_point->id;
          mate_marker.scale.x = 0.02;
          mate_marker.scale.y = 0.02;
          mate_marker.scale.z = 0.01;
          mate_marker.color.r = 0.0;
          mate_marker.color.g = 0.0;
          mate_marker.color.b = 1.0;
          mate_marker.color.a = 0.25;
          female_mate_markers.markers.push_back(mate_marker);
        }

      }

      male_mate_pub_.publish(male_mate_markers);
      female_mate_pub_.publish(female_mate_markers);
      wrenches_pub_.publish(wrench_markers);
      mate_details_pub_.publish(mate_details_markers);
    }

    // TODO: move this introspection out of this thread
    if (publish_active_mates_) {
      active_mates_pub_.publish(mates_msg);
      mating_pub_.publish(mating_msg);
    }

  }

  AssemblySoup::~AssemblySoup() {
    running_ = false;
    state_update_thread_.join();
    gzmsg<<"Assembly soup cleaning up."<<std::endl;
  }

  void AssemblySoup::stateUpdateLoop() {

    gzwarn << "State update thread running!" << std::endl;

    gazebo::physics::WorldPtr world = this->model_->GetWorld();
    gazebo::common::Time now(0);
    gazebo::common::Time update_period(1.0/updates_per_second_);
    gazebo::common::Time last_update_time = world->SimTime();

    while(running_) {

      now = world->SimTime();

      if(now < last_update_time + update_period) {
        gazebo::common::Time::Sleep(last_update_time + update_period - now);
      } else {
        last_update_time = world->SimTime();
        this->queueStateUpdates();
      }
    }
  }

  // Called by the world update start event
  // This is where the logic that connects and updates joints needs to happen
  void AssemblySoup::OnUpdate(const gazebo::common::UpdateInfo & _info)
  {

    if (!running_) {
      this->queueStateUpdates();

      gzwarn << "Starting thread..." << std::endl;
      state_update_thread_ = boost::thread(boost::bind(&AssemblySoup::stateUpdateLoop, this));
      running_ = true;
      gzwarn << "Started." <<std::endl;
    }

    if(last_update_time_ == gazebo::common::Time::Zero) {
      last_update_time_ = _info.simTime;
      return;
    }

    // Try to lock mutex in order to change mate constraints
    {
      boost::mutex::scoped_lock update_lock(update_mutex_, boost::try_to_lock);
      while(not mate_update_queue_.empty()) {
        //gzwarn<<"updating mate "<<mate_update_queue_.front()->getDescription()<<std::endl;
        mate_update_queue_.front()->updateConstraints();
        mate_update_queue_.pop();
      }
    }

    // Reset forces on all atoms
    for(std::vector<AtomPtr>::iterator it_a = atoms_.begin();
        it_a != atoms_.end();
        ++it_a)
    {
      AtomPtr atom = *it_a;
      atom->wrench = KDL::Wrench::Zero();
    }

    // Get the timestep and time
    gazebo::common::Time timestep = _info.simTime - last_update_time_;
    gazebo::common::Time now = gazebo::common::Time::GetWallTime();
 
    // Compute forces on all atoms
    for (boost::unordered_set<MatePtr>::iterator it = mates_.begin();
         it != mates_.end();
         ++it)
    {
      MatePtr mate = *it;
      mate->update(timestep);
    }
    static const double a = 0.95;
    static double dt = 0;
    dt = (1.0-a)*(gazebo::common::Time::GetWallTime() - now).Double() + (a) * dt;
    //gzwarn<<"update: "<<dt<<std::endl;

    last_update_time_ = _info.simTime;
  }

  // Register this plugin with the simulator
  GZ_REGISTER_MODEL_PLUGIN(AssemblySoup);
}

