// Header-only shared TA storage used for best-effort exchange between scheduler and estimator.
#pragma once

#include <atomic>
#include <mutex>
#include <ctime>

namespace srsran {

struct ta_last_info {
  inline static std::mutex mtx{};
  inline static long long unsigned int time_now = -1;
  inline static int last_ta = 99;
  inline static bool rar_ta = false;
  inline static uint16_t last_crnti = 0;
};

inline void set_last_ta(long long unsigned int time_now, int new_ta, bool is_rar_ta ) {
  std::lock_guard<std::mutex> l(ta_last_info::mtx);
  ta_last_info::time_now = time_now;
     if(is_rar_ta){
        //reset 
        ta_last_info::last_ta = new_ta;
    }

     else{
     int temp_ta = ta_last_info::last_ta;
     temp_ta += new_ta;
    ta_last_info::last_ta = temp_ta;
  }
    ta_last_info::rar_ta = is_rar_ta;
}

inline void set_last_ta_with_crnti(long long unsigned int time_now, int new_ta, bool is_rar_ta, uint16_t c_rnti) {
  std::lock_guard<std::mutex> l(ta_last_info::mtx);
  ta_last_info::time_now = time_now;
  ta_last_info::last_crnti = c_rnti;
     if(is_rar_ta){
        //reset 
        ta_last_info::last_ta = new_ta;
    }

     else{
     int temp_ta = ta_last_info::last_ta;
     temp_ta += new_ta;
    ta_last_info::last_ta = temp_ta;
  }
    ta_last_info::rar_ta = is_rar_ta;
}

inline bool get_last_ta(long long unsigned int &time_now, int &new_ta, bool &is_rar_ta) {
  std::lock_guard<std::mutex> l(ta_last_info::mtx);
  time_now = ta_last_info::time_now;
  new_ta = ta_last_info::last_ta;
  is_rar_ta = ta_last_info::rar_ta;
  return true;
}

inline bool get_last_ta_with_crnti(long long unsigned int &time_now, int &new_ta, bool &is_rar_ta, uint16_t &c_rnti) {
  std::lock_guard<std::mutex> l(ta_last_info::mtx);
  time_now = ta_last_info::time_now;
  new_ta = ta_last_info::last_ta;
  is_rar_ta = ta_last_info::rar_ta;
  c_rnti = ta_last_info::last_crnti;
  return (c_rnti != 0); // Return false if no C-RNTI set
}

} // namespace srsran
