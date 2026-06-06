//
// Created by awalol on 2026/4/5.
//

#ifndef DS5_DONGLE_LINUX_UTILS_H
#define DS5_DONGLE_LINUX_UTILS_H
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <vector>


class Utils {
public:
    static void print_hex(const std::vector<uint8_t> data);
    static void print_hex(const uint8_t* data,size_t size) ;
};



#endif //DS5_DONGLE_LINUX_UTILS_H
