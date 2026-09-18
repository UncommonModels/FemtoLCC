# The FemtoLCC application.
#
# The .cpp files below are the hardware drivers. They are written against a
# small slice of the Arduino API rather than ESP-IDF directly, because that is
# the API they were first written for; Arduino.h, Wire.h and Config.h in this
# directory declare exactly what they use and ArduinoCompat.cxx implements it
# over ESP-IDF. They are ordinary sources of this firmware: edit them here.

idf_component_register(
    SRCS
        "main.cxx"
        "FemtoController.cxx"
        "FemtoBits.cxx"
        "LiveControl.cxx"
        "ModuleStore.cxx"
        "ArduinoCompat.cxx"
        "Config.cxx"
        # The hardware drivers; see the note above.
        "Channels.cpp"
        "Occupancy.cpp"
        "Turnouts.cpp"
        "IoPins.cpp"
        "Expander.cpp"
        "I2cBus.cpp"
        "IoBoards.cpp"
        "ServoBoards.cpp"
        "DCCSource.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        openmrn
        driver
        esp_adc
        # esp_mac.h: the node ID is derived from the chip's base MAC.
        esp_hw_support
        esp_wifi
        nvs_flash
        spiffs
        vfs
)

# ESP-IDF 5.2 deprecates the legacy I2C driver that ArduinoCompat uses; the
# replacement is not available on every target this project can be built for.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-declarations)
