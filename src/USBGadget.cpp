#include <iostream>
#include <fstream>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <dirent.h>

#include "USBGadget.h"

#include <filesystem>

namespace {
constexpr const char* kAudioFunctionName = "uac2.gs0";
constexpr const char* kHidFunctionName = "hid.usb0";

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value && std::string(value) != "0";
}
}

bool USBGadget::write_file(const std::string& path, const std::string& value) {
    std::ofstream file(path);
    if (!file) {
        std::cerr << "Failed to open " << path << " for write: " << strerror(errno) << std::endl;
        return false;
    }

    file << value;
    if (!file.good()) {
        std::cerr << "Failed to write " << path << " value " << value << std::endl;
        return false;
    }
    return true;
}

bool USBGadget::make_dir(const std::string& path) {
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool USBGadget::remove_dir(const std::string& path) {
    return rmdir(path.c_str()) == 0 || errno == ENOENT;
}

std::string USBGadget::find_udc() {
    DIR* dir = opendir("/sys/class/udc");
    if (!dir) {
        return "";
    }

    dirent* entry;
    std::string udc;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] != '.') {
            udc = entry->d_name;
            break;
        }
    }
    closedir(dir);
    return udc;
}

USBGadget::USBGadget(const std::string& name)
    : gadget_root("/sys/kernel/config/usb_gadget/" + name) {}

bool USBGadget::create() {
    if (geteuid() != 0) {
        std::cerr << "Need root privileges" << std::endl;
        return false;
    }

    udc_name = find_udc();
    if (udc_name.empty()) {
        std::cerr << "UDC controller not found" << std::endl;
        return false;
    }

    destroy();

    if (!make_dir(gadget_root)) {
        return false;
    }

    if (!write_file(gadget_root + "/idVendor", "0x054C") ||
        !write_file(gadget_root + "/idProduct", "0x0CE6") ||
        !write_file(gadget_root + "/bcdDevice", "0x0100") ||
        !write_file(gadget_root + "/bcdUSB", "0x0200")) {
        return false;
    }

    if (!make_dir(gadget_root + "/strings/0x409") ||
        !write_file(gadget_root + "/strings/0x409/serialnumber", "1234567890") ||
        !write_file(gadget_root + "/strings/0x409/manufacturer", "Sony Interactive Entertainment") ||
        !write_file(gadget_root + "/strings/0x409/product", "DualSense Wireless Controller")) {
        return false;
    }

    if (!make_dir(gadget_root + "/configs/c.1") ||
        !write_file(gadget_root + "/configs/c.1/MaxPower", "500") ||
        !write_file(gadget_root + "/configs/c.1/bmAttributes", "0xC0")) {
        return false;
    }

    const std::string uac_func = gadget_root + "/functions/" + kAudioFunctionName;
    if (!make_dir(uac_func) ||
        !write_file(uac_func + "/c_chmask", "0x33") ||
        !write_file(uac_func + "/c_ssize", "2") ||
        !write_file(uac_func + "/c_srate", "48000") ||
        !write_file(uac_func + "/p_chmask", "0x1") ||
        !write_file(uac_func + "/p_ssize", "2") ||
        !write_file(uac_func + "/p_srate", "48000") ||
        !write_file(uac_func + "/p_volume_min", "256") ||
        !write_file(uac_func + "/p_volume_max", "512") ||
        !write_file(uac_func + "/p_volume_res", "256")) {
        return false;
    }

    if (std::filesystem::exists(uac_func + "/function_name") &&
        !write_file(uac_func + "/function_name", "DualSense Wireless Controller")) {
        return false;
    }

    const std::string hid_func = gadget_root + "/functions/" + kHidFunctionName;
    const bool no_out_endpoint = env_enabled("DS5_HID_NO_OUT_ENDPOINT");
    if (!make_dir(hid_func) ||
        !write_file(hid_func + "/protocol", "0") ||
        !write_file(hid_func + "/subclass", "0") ||
        !write_file(hid_func + "/report_length", "64") ||
        !write_file(hid_func + "/no_out_endpoint", no_out_endpoint ? "1" : "0")) {
        return false;
    }

    std::ofstream report(hid_func + "/report_desc", std::ios::binary);
    if (!report) {
        return false;
    }

    report.write(reinterpret_cast<const std::ostream::char_type *>(desc_hid_report.data()), desc_hid_report.size());
    if (!report.good()) {
        return false;
    }
    report.close();

    if (symlink(hid_func.c_str(), (gadget_root + "/configs/c.1/" + kHidFunctionName).c_str()) != 0 ||
        symlink(uac_func.c_str(), (gadget_root + "/configs/c.1/" + kAudioFunctionName).c_str()) != 0) {
        return false;
    }

    sleep(1);

    if (!write_file(gadget_root + "/UDC", udc_name)) {
        destroy();
        return false;
    }

    std::cout << "Composite gadget created: UAC2 + HID" << std::endl;
    return true;
}

void USBGadget::destroy() {
    const std::string udc_path = gadget_root + "/UDC";
    std::ifstream check(udc_path);
    if (check.good()) {
        write_file(udc_path, "");
    }

    unlink((gadget_root + "/configs/c.1/" + kHidFunctionName).c_str());
    unlink((gadget_root + "/configs/c.1/" + kAudioFunctionName).c_str());

    remove_dir(gadget_root + "/functions/" + kHidFunctionName);
    remove_dir(gadget_root + "/functions/" + kAudioFunctionName);
    remove_dir(gadget_root + "/configs/c.1");
    remove_dir(gadget_root + "/strings/0x409");
    remove_dir(gadget_root);
}

bool USBGadget::exists() const {
    if (std::filesystem::exists(gadget_root)) {
        if (!std::filesystem::exists(gadget_root + "/configs/c.1/" + kAudioFunctionName)) {
            return false;
        }

        std::ifstream file(gadget_root + "/UDC");
        if (!file.is_open()) {
            return false;
        }
        std::string line;
        if (getline(file,line)) {
            if (!line.empty()) {
                return true;
            }
        }
    }

    return false;
}
