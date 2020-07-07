# Smart Anti-Flood System
This is a kind of ~~magic~~ home anti-flood system. 

It contains valve with actuator, hall sensor as a flow meter and ESP32 as a main microcontroller. 
Water flow is continuously measured and on *some conditions* ESP32 decides when to close the main valve.
It may depend either on fixed rules (e.g. flow speed/duration) or anomaly detection algorithm. 


![collage](doc/img/collage.jpg)


## Features
- installed on main water connection it measures consumption amount and flow duration
- telemetry (start time, water consumption amount, flow duration and speed) is on-the-fly sent to Google Cloud Platform for further processing and analysing
- in case of anomaly detection main valve is closing to prevent flooding
- user is notified about all events and can decide whether open or close the main valve remotely
- collected data may be used to
  - make an analysis that helps to **save more water**
  - learn model of typical water usage and improve anomaly detection algorithm
  - generate beautiful reports e.g. on Google Data Studio  

## Hardware


| ![hardware](doc/img/hardware.jpg) | 
|:--:| 
| *Valve with actuator and hall sensor as a flow meter* |

![pcb-front](doc/img/pcb-front.jpg)

*PCB's front...*

![pcb-back](doc/img/pcb-back.jpg)

*... and back*

## Software

### Firmware
- written in C under [esp-idf: official development framework for the ESP32 and ESP32-S Series SoCs](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/).

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

## TODO
* implement standalone, local algorithm for [anomaly detection using the multivariate gaussian distribution](https://www.coursera.org/lecture/machine-learning/anomaly-detection-using-the-multivariate-gaussian-distribution-DnNr9) - like in my other project https://github.com/tomekceszke/ml-login...
* ... or use GCP solution https://cloud.google.com/ai-platform/docs/getting-started-tensorflow-estimator 
* improve logging (store locally on NVS and on the cloud https://cloud.google.com/iot/docs/how-tos/device-logs?hl=en_US)
* prepare basic web interface (status, open/close valve)
* send state to the cloud https://cloud.google.com/iot/docs/how-tos/config/getting-state?hl=en_US#http_bridge
* allow to control device from the cloud (https://cloud.google.com/iot/docs/how-tos/config/configuring-devices?hl=en_US and https://cloud.google.com/iot/docs/how-tos/commands?hl=en_US)
* optimize cloud costs (decrease equipment availability and performance etc...)
* mobile app - send push notification with option to open the valve
* hardware - add reset button on case 
* fix memory leaks ( ͡° ͜ʖ ͡°)