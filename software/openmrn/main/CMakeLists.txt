# The FemtoLCC application.
#
# Channels.cpp, Occupancy.cpp, Turnouts.cpp, IoPins.cpp and Expander.cpp are
# byte-identical copies of the hardware drivers in ../../FemtoLCC — see
# ../sync-drivers.sh. They compile unchanged because Arduino.h, Wire.h and
# Config.h in this directory supply what they expect; ArduinoCompat.cxx
# implements the first two over ESP-IDF.

idf_component_register(
    SRCS
        "main.cxx"
        "FemtoController.cxx"
        "ArduinoCompat.cxx"
        # Copies from ../../FemtoLCC — do not edit here.
        "Channels.cpp"
        "Occupancy.cpp"
        "Turnouts.cpp"
        "IoPins.cpp"
        "Expander.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        openmrn
        driver
        esp_adc
        esp_wifi
        nvs_flash
        spiffs
        vfs
)

# ESP-IDF 5.2 deprecates the legacy I2C driver that ArduinoCompat uses; the
# replacement is not available on every target this project can be built for.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-declarations)
