#ifndef PARAM_LOADER_H
#define PARAM_LOADER_H

#include <ros/ros.h>
#include <dynamic_reconfigure/server.h>
#include <string>
#include <iostream>

bool load_successful = true;

template <typename T>
T load_param(ros::NodeHandle &nh, const std::string &name, const T &default_value, bool optional = true, bool print_value = true)
{
  T loaded = default_value;
  bool success = nh.getParam(name, loaded);
  if (success)
  {
    if (print_value)
      std::cout << "\t" << name << ":\t" << loaded << std::endl;
  } else
  {
    loaded = default_value;
    if (!optional)
    {
      ROS_ERROR("Could not load non-optional parameter %s", name.c_str());
      load_successful = false;
    } else
    {
      std::cout << "\t" << name << ":\t" << loaded << std::endl;
    }
  }
  return loaded;
}

template <typename T>
T load_param_compulsory(ros::NodeHandle &nh, const std::string &name, bool print_value = true)
{
  return load_param(nh, name, T(), false, print_value);
}

template <typename ConfigType>
class DynamicReconfigureMgr
{
  public:
    // this variable holds the latest received configuration
    ConfigType config;
    // the callback itself
    void dynamic_reconfigure_callback(ConfigType& new_config, uint32_t level)
    {
      ROS_INFO("Dynamic reconfigure request received:");
      if (m_print_values)
      {
        print_changed_params(new_config);
      }
      m_not_initialized = false;
      config = new_config;
    };
    // initialize some stuff in the constructor
    DynamicReconfigureMgr(bool print_values = true)
    {
      m_print_values = print_values;
      m_not_initialized = true;
      // initialize the dynamic reconfigure callback
      m_cbf = boost::bind(&DynamicReconfigureMgr<ConfigType>::dynamic_reconfigure_callback, this, _1, _2);
      m_server.setCallback(m_cbf);
    };

  private:
      bool m_print_values, m_not_initialized;
      // dynamic_reconfigure server variables
      typename dynamic_reconfigure::Server<ConfigType> m_server;
      typename dynamic_reconfigure::Server<ConfigType>::CallbackType m_cbf;

      // method for printing names and values of new received parameters (prints only the changed ones)
      void print_changed_params(const ConfigType& new_config)
      {
        // Note that this part of the API is still unstable and may change! It was tested with ROS Kinetic
        std::vector<typename ConfigType::AbstractParamDescriptionConstPtr> descrs = new_config.__getParamDescriptions__();
        for (auto &descr : descrs)
        {
          boost::any val, old_val;
          descr->getValue(new_config, val);
          descr->getValue(config, old_val);
          // try to guess the correct value (these should be the only ones supported)
          int *intval;
          double *doubleval;
          bool *boolval;
          std::string *stringval;

          if (try_cast(val, intval))
          {
            if (!try_compare(old_val, intval) || m_not_initialized)
              std::cout << "\t" << descr->name << ":\t" << *intval << std::endl;
          } else if (try_cast(val, doubleval))
          {
            if (!try_compare(old_val, doubleval) || m_not_initialized)
              std::cout << "\t" << descr->name << ":\t" << *doubleval << std::endl;
          } else if (try_cast(val, boolval))
          {
            if (!try_compare(old_val, boolval) || m_not_initialized)
              std::cout << "\t" << descr->name << ":\t" << *boolval << std::endl;
          } else if (try_cast(val, stringval))
          {
            if (!try_compare(old_val, stringval) || m_not_initialized)
              std::cout << "\t" << descr->name << ":\t" << *stringval << std::endl;
          } else
          {
            std::cout << "\t" << descr->name << ":\t" << "unknown dynamic reconfigure type" << std::endl;
          }
        }
      }
      // helper methods for automatic parameter value parsing
      template <typename T>
      inline bool try_cast(boost::any& val, T*& out)
      {
        return (out = boost::any_cast<T>(&val));
      };
      template <typename T>
      inline bool try_compare(boost::any& val, T*& to_what)
      {
        T* tmp;
        if ((tmp = boost::any_cast<T>(&val)))
        {
          /* std::cout << std::endl << *tmp << " vs " << *to_what << std::endl; */
          return *tmp == *to_what;
        } else
        { // the value should not change during runtime - this should never happen (but its better to be safe than sorry)
          ROS_WARN("Value type has changed - this should not happen!");
          return false;
        }
      };
};

#endif // PARAM_LOADER_H
