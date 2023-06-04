# FormationFlight  
A LoRa/ESPNOW inter-UAS communication system that has been forked from INAV-Radar. FormationFlight adds many improvements including support for some of the tiny and inexpensive targets developed for the [ExpressLRS project](https://github.com/ExpressLRS/ExpressLRS). All of these impovements make this project more accessable and will allow it to mature more quickly.
  
    
    
![Logo](https://github.com/mistyk/inavradar-ESP32/raw/master/docs/logo.png)

INAV-Radar is an addition to the [INAV](https://github.com/iNavFlight/inav) flight control software, it relays information about UAVs in the area to the flight controller for display on the OSD. INAV-Radar does this by using [LoRa](https://en.wikipedia.org/wiki/LoRa) radio to broadcast position, altitude, speed and plane name. It also listens for other UAVs so INAV OSD  can display this information as a HUD.

# [All the documentation is available here at RCgroups](https://www.rcgroups.com/forums/showthread.php?3304673-iNav-Radar-ESP32-LoRa-modems)




![Overview](https://i.imgur.com/LlABFDV.png)

[Facebook Group](https://www.facebook.com/groups/360607501179901/)

# * FormationFlight Information

# Targets:
This is a work in progress and there is no guarantee your target will work... However, please check contents of .ini files within the targets folder to see some of the boards that are being tested.

# Building:
Visual Studio Code with PlatformIO is required.

# Flashing:
Initial firmware can be flashed via UART. It may be necessary to short boot pads or hold the boot button of your target during power up.

# Wifi defaults:
SSID: "iNav Radar-XXXXXX" (XXXXXX is a device specific ID -based on MAC address?)  
Password: inavradar  
Device IP: 192.168.4.1  

# Connecting to flight controller:
Configure MSP protocol on an unused UART of the flight controller. Connect your target ground, TX, and RX pads to the associated UART pads on the flight controller. Provide power to your FormationFlight target as needed. Additional options may be available in your flight controller to enable and adjust OSD.

# Usage WARNING: 
Modern model aircraft often depend upon the reliability of more than one radio signal. Extensive testing is required to verify all models will function correctly as a fleet. NEVER power up a model while other models are in the air!

