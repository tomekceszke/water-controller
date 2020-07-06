# Anti-flood system
It is a kind of DIY home's anti-flood system. 

It contains valve with actuator, hall sensor as a flow meter and ESP32 as a main microcontroller. 
Water flow is continuously measured and on *some conditions* ESP32 decides to close the main valve.


![collage](doc/img/collage.jpg)

## Hardware

![hardware](doc/img/hardware.jpg)

*Valve with actuator and hall sensor as a flow meter*

![pcb-front](doc/img/pcb-front.jpg)

*PCB's front...*

![pcb-back](doc/img/pcb-back.jpg)

*... and back*

## Software
tbd

### 3rd Party Components
- udp-logging - https://github.com/MalteJ/embedded-esp32-component-udp_logging - Apache License v2.0
- base64 decoder - https://github.com/zhicheng/base64 - Public Domain
- esp-idf examples - https://github.com/espressif/esp-idf/tree/master/examples - Apache License v2.0
- ota-server - tbd


## Testing

![assembling](doc/img/assembling.jpg)

*Testing PCB "on the desk"*

![monitoring](doc/img/monitoring.jpg)

*Water is flowing*

![activated](doc/img/protected.jpg)

*Protection activated*

## Collecting and analyzing data
![big-query](doc/img/big-query.png)

*Collected data on Google BigQuery database*

![report1](doc/img/report1.png)

*Sample report on Google Data Studio*