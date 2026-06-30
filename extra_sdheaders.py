Import("env")
import os

fw_dir = env.PioPlatform().get_package_dir("framework-arduinoadafruitnrf52")
if fw_dir:
    sd_dir = os.path.join(fw_dir, "components", "softdevice", "s140", "headers")
    env.Append(CPPPATH=[sd_dir, os.path.join(sd_dir, "nrf52")])
