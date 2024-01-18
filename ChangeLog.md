Release 3.1.1:
  - fix auto reconnect to WIFI (issue #21)
  - fix insecure unencrypted AccesspointMode
  - fix crash of ESP32 by using Solax-X3 settings (issue #22)
  - add support for ESP32-S2, -S3, -C3 (issue #20)
  - add optional register field: "valueAdd" to add/sub a static value (issue #32)
  - add "object_id" to jsondata of /getitems (issue #36)
  - add registers of Deye SUN-xx-SG04LP3 (issue #32)
  
Release 3.1.0:  
  - add Sofar-KTL Solarmax-SGA
  - bugfix: memoryleak
  - add a "mapping" for items
  - SOLAX-MIC added
  - bugfix: modbusitems sometime truncated
  - add official Releases Info

Release 3.0.0:  
  actual stable release

  - Solax X1
  - Solax X3
  - Growatt SPH

Release 2.0.0:  
  latest release, full working with Solax X1
  Solax X3 needs to configure in register.h. If you done that, feel free to make a pull request

Release 1.0.0
  first runnable version:
  - only with livedata
  - configurable data in webfrontend
  - Transmission Interval
  - Modbus Client-ID
  - Modbus Baudrate
  - Wifi Credentials via Wifimanager
  - MQTT Server, Port and optional Credentials
  - MQTT Basepath and Hostname

 Status Overview at webpage
  - IPAdresse
  - WiFiName
  - Wifi RSSI
  - MQTT Status
  - Uptime
  - Free Memory

 And Buttons to:
  - Reboot
  - Reset
  - WiFi Credentials Reset
   

