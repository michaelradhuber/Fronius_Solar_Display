# netscanner-lib/netscanner-lib/README.md

# NetScanner Library

The NetScanner library provides functionality for scanning the network and managing the ARP table on ESP32 devices. It is designed to work with FreeRTOS and utilizes the ESP-IDF framework for networking capabilities.

## Features

- Send ARP requests to discover devices on the network.
- Read and manage the ARP table.
- Print the ARP table entries, including IP, MAC address, and vendor information.

## Installation

1. Clone the repository:

   ```
   git clone <repository-url>
   ```

2. Navigate to the project directory:

   ```
   cd netscanner-lib
   ```

3. Create a build directory and navigate into it:

   ```
   mkdir build
   cd build
   ```

4. Run CMake to configure the project:

   ```
   cmake ..
   ```

5. Build the project:

   ```
   make
   ```

## Usage

To use the NetScanner library in your project, include the header file:

```cpp
#include "netscanner.h"
```

You can create an instance of the `NetScanner` class and call its methods to perform network scanning and manage the ARP table.

## Dependencies

- ESP-IDF framework
- FreeRTOS

## Configuration

Ensure that your ESP32 device is properly configured for Wi-Fi connectivity before using the NetScanner library. You may need to adjust the Wi-Fi settings in your project to connect to the desired network.

## License

This project is licensed under the MIT License. See the LICENSE file for more details.