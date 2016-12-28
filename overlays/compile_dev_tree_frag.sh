#!/bin/bash
mkdir -p build
dtc -I dts -O dtb -o ./build/axi_pwm.dtbo -@ pwm.dts
dtc -I dts -O dtb -o ./build/axi_random.dtbo -@ random.dts
dtc -I dts -O dtb -o ./build/axi_sw.dtbo -@ sw.dts
dtc -I dts -O dtb -o ./build/axi_timer.dtbo -@ timer.dts
dtc -I dts -O dtb -o ./build/axi_id_reg.dtbo -@ id_reg.dts