/*******************************************************************************
 * Copyright (C) 2023 TTTech Auto AG. All rights reserved                      *
 * Operngasse 17-21, 1040 Vienna, Austria. office(at)tttech-auto.com           *
 ******************************************************************************/
#include <chrono>
#include <iostream>
#include <string>

#include "data_flow_scheduler/data_flow_executor.h" // namespace DFS_Interface

using namespace DFS_Interface;

DFSExecutor::DFSExecutor(const std::string node_name_, TimerInfoVector Timers_)
{
  node_name = node_name_;
  timers_ = Timers_;
  init();
}

DFSExecutor::DFSExecutor(const std::string node_name_, TopicInfoVector Callbacks_)
{
  node_name = node_name_;
  topics_ = Callbacks_;
  init();
}

DFSExecutor::DFSExecutor(const std::string node_name_, TimerInfoVector Timers_,
                         TopicInfoVector Callbacks_)
{
  node_name = node_name_;
  timers_ = Timers_;
  topics_ = Callbacks_;
  init();
}

// Initialize the DFSExecutor object
void DFSExecutor::init()
{
  std::cout << "DFSExecutor Info:\n--\n";
  for (const auto &Timer_ : timers_)
  {
    // Store timer information
    node_info.callback_topic_name.push_back(Timer_.name);
    node_info.runtime.push_back(Timer_.runtime);
    node_info.callback_id.push_back(Timer_.id);
    node_info.callback_type.push_back(Timer_.type);
    node_info.pub_topic_name.push_back(Timer_.pub);

    // Print timer information
    std::cout << "TIMER: \n";
    std::cout << " Name: " << Timer_.name << std::endl;
    std::cout << " Runtime: " << Timer_.runtime << std::endl;
    std::cout << " ID: " << Timer_.id << std::endl;
    std::cout << " Type: " << Timer_.type << std::endl;
    std::cout << "PUB: " << Timer_.pub << std::endl;
  }

  for (const auto &Callback_ : topics_)
  {
    // Store callback information
    node_info.callback_topic_name.push_back(Callback_.name);
    node_info.runtime.push_back(Callback_.runtime);
    node_info.callback_id.push_back(Callback_.id);
    node_info.callback_type.push_back(Callback_.type);
    node_info.pub_topic_name.push_back(Callback_.pub);

    // Print callback information
    std::cout << "TOPIC: \n";
    std::cout << " Name: " << Callback_.name << std::endl;
    std::cout << " Runtime: " << Callback_.runtime << std::endl;
    std::cout << " ID: " << Callback_.id << std::endl;
    std::cout << " Type: " << Callback_.type << std::endl;
    std::cout << "PUB: " << Callback_.pub << std::endl;
  }
  std::cout << "\n--\n";

  RCLCPP_INFO(rclcpp::get_logger(node_name), "DFSExecutor Created.");

  // Initialize the DFS executor with callbacks and timers
  cb_handler.init(node_name, topics_, timers_);

  // Connect to the server
  if (!client.connect(node_name))
  {
    RCLCPP_ERROR(rclcpp::get_logger(node_name + "|DFSExecutor"), "Connecting to Server failed");
  }

  if (!send_callback_infos())
  {
    RCLCPP_ERROR(rclcpp::get_logger(node_name + "|DFSExecutor"), "Sending Infos message to Server failed");
  }
}

// Spin function that runs in a separate thread and executes read_msgs based on incoming Execute_Info
void DFSExecutor::spin(std::vector<std::function<void()>> &functionVector, std::mutex &vectorMutex)
{

  std::vector<std::thread *> child_thread(node_info.callback_topic_name.size());

  while (true)
  {
    Execute_Info execute_;
    int read_ = client.read_raw_data(&execute_, sizeof(Execute_Info));
    int j = execute_.mtx_id;

    if (read_ < 0)
    {
      RCLCPP_ERROR(rclcpp::get_logger(node_name + "|DFSExecutor"), "Could not read from socket.");
      break;
    }
    else
    {
      if (child_thread[j] != nullptr)
      {
        if (child_thread[j]->joinable())
        {
          // Detach the previously joined child thread
          // std::cout << "Detaching thread " << j << "\n";
          // RCLCPP_INFO(rclcpp::get_logger(node_name + "|DFSExecutor"), "Detaching thread %d", j);
          auto pt = child_thread[j];
          detached_threads.push_back(pt);
          pt->detach();

          child_thread[j] = nullptr;
        }
      }
      // Create a new child thread to execute the read_msgs function
      child_thread[j] = new std::thread(&DFSExecutor::read_msgs, this, execute_, std::ref(functionVector), std::ref(vectorMutex));
      // std::cout << "Created thread " << j << "\n";
      // RCLCPP_INFO(rclcpp::get_logger(node_name + "|DFSExecutor"), "Created new thread %d", j);
      if (SET_THREAD_PRIORITY)
      {
        if (child_thread[j] != nullptr)
        {
          setThreadPriority(*child_thread[j], 2, node_name);
        }
      }
    }
  }
}

// Serialize the buffer containing callback information
std::string DFSExecutor::serialize_buffer() const
{
  std::string result;

  for (const auto &topic_name : node_info.callback_topic_name)
  {
    result += topic_name + ",";
  }
  result += ";";
  for (const auto &runtime : node_info.runtime)
  {
    result += std::to_string(runtime) + ",";
  }
  result += ";";
  for (const auto &topic_name : node_info.pub_topic_name)
  {
    result += topic_name + ",";
  }
  result += ";";
  for (const auto &id : node_info.callback_id)
  {
    result += std::to_string(id) + ",";
  }
  result += ";";
  for (const auto &type : node_info.callback_type)
  {
    result += std::to_string(type) + ",";
  }
  return result;
}

// Send callback information to the server
bool DFSExecutor::send_callback_infos()
{
  std::string n_buffer = serialize_buffer();
  return client.send_data(n_buffer);
}

// Function executed by child threads to read messages and execute callbacks
void DFSExecutor::read_msgs(Execute_Info execute_,
                            std::vector<std::function<void()>> &functionVector,
                            std::mutex &vectorMutex)
{
  //(zayas) TODO check for thread safety of the parameters"
  std::thread T_(
      &DFS_Interface::CallbackHandler::run_callback,
      &cb_handler,
      execute_.mtx_id,
      execute_.callb,
      execute_.type);
  if (SET_THREAD_PRIORITY)
  {
    setThreadPriority(T_, 2, node_name);
  }

  {
    bool finish = false;
    std::unique_lock<std::mutex> lock(cb_handler.timeout_condition[execute_.mtx_id]->mtx_);
    const auto time_point = std::chrono::high_resolution_clock::now();
    bool has_timeout = false;
    do
    {
      // calculate how much time elapsed
      double elapsed_time_us = std::chrono::duration<double, std::micro>(std::chrono::system_clock::now() - time_point).count();
      int elapsed = (int)round(elapsed_time_us);
      auto wait_time = std::chrono::microseconds(execute_.runtime - elapsed);
      if (elapsed >= execute_.runtime)
      {
        // timeout!
        has_timeout = true;
        break;
      }

      const auto timep = std::chrono::system_clock::now() + wait_time;

      const std::cv_status status = cb_handler.timeout_condition[execute_.mtx_id]->cvar_.wait_until(lock, timep);
      if (status == std::cv_status::timeout)
      {
        // std::cout << "timeout" << std::endl;
        finish = true;
        has_timeout = true;
        break;
      }
      else
      {
        if (cb_handler.timeout_condition[execute_.mtx_id]->finished_.load())
        {
          finish = true;
          has_timeout = false;
          break;
        }
        else
        {
          // spurious
        }
      }
    } while (finish == false);

    if (has_timeout)
    {
      RCLCPP_WARN(rclcpp::get_logger(node_name), "Timeout occurred. Function did not finish in time.");
      execute_.timeout = true;
      execute_.suc = cb_handler.timeout_condition[execute_.mtx_id]->suc_;
      client.send_raw_data(&execute_, sizeof(Execute_Info));
    }
    else
    {
      std::lock_guard<std::mutex> lock(vectorMutex);
      if (!functionVector.empty() && cb_handler.timeout_condition[execute_.mtx_id]->suc_)
      {
        functionVector[execute_.callb]();
        functionVector[execute_.callb] = []() {};
      }

      execute_.timeout = false;
      execute_.suc = cb_handler.timeout_condition[execute_.mtx_id]->suc_;
      client.send_raw_data(&execute_, sizeof(Execute_Info));
    }
  }

  if (T_.joinable())
    T_.detach();
}