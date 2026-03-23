# Hardware Abstraction Layer — Zeros/DereZ Unified API

> One API. Screen or robot. Same function calls.
>
> **Date**: March 22, 2026
> **Status**: Specification — for kernel and Z+ agents
> **Owner**: Interface layer (this is the bridge between kernel and Z+)

---

## Core Principle

A game entity and a robot actuator are the same thing: a signal chain
node that receives input, processes it, and produces output.

```
// Game: enemy chases player
if sense(player, within: 100) -> move_toward(player, speed: 5)

// Robot: follow a line
if sense(line_sensor, within: 100) -> move_toward(target, speed: 5)
```

Same pattern. Same API. Different hardware underneath.

---

## The Unified Entity API

Every "thing" in Zeos — game sprite, robot motor, sensor, screen pixel —
is a **signal node** with these standard ports:

### Input Ports (sense)
| Port      | Game Use              | Robot Use                |
|-----------|-----------------------|--------------------------|
| `position`| sprite x,y,z          | GPS/encoder position     |
| `distance`| collision radius      | ultrasonic/lidar reading  |
| `contact` | collision event       | bumper switch            |
| `angle`   | sprite rotation       | IMU heading              |
| `heat`    | damage/health         | motor current/temp       |
| `signal`  | event from another    | I2C/SPI/CAN data        |

### Output Ports (act)
| Port      | Game Use              | Robot Use                |
|-----------|-----------------------|--------------------------|
| `move`    | translate sprite      | drive motors             |
| `turn`    | rotate sprite         | steer servo              |
| `emit`    | spawn projectile      | activate solenoid/LED    |
| `voice`   | play sound            | speaker/buzzer           |
| `signal`  | event to another      | I2C/SPI/CAN write        |

### Standard Functions
```
sense(port, [within: radius])   -> value
move_toward(target, speed: n)   -> signal
turn_to(angle)                  -> signal
emit(type, direction, speed)    -> signal
distance_to(target)             -> value
is_touching(target)             -> bool
```

---

## Hardware Drivers as Signal Nodes

Each hardware driver registers itself as a signal node type:

```c
/* In kernel — motor driver registers as a signal node */
struct hw_driver motor_driver = {
    .name       = "pwm_motor",
    .bus        = HW_BUS_GPIO,
    .input      = {"speed", "direction"},
    .output     = {"current", "fault"},
    .process    = motor_process,
};
hw_register(&motor_driver);
```

The Z+ runtime doesn't know or care if a node is a sprite renderer
or a motor controller. It just sends signals and reads outputs.

---

## Sensor Types (Zeros)

For robotics, these sensor abstractions map to real hardware:

| Abstraction    | Hardware Examples           | Signal Type    |
|----------------|-----------------------------|----------------|
| `distance`     | HC-SR04, VL53L0X, LIDAR     | uint32 (mm)    |
| `line`         | TCRT5000 array, QTR-8       | uint8[] (vals) |
| `color`        | TCS34725                    | rgb struct     |
| `accel`        | MPU6050, LSM6DS3            | xyz struct     |
| `gyro`         | MPU6050, LSM6DS3            | xyz struct     |
| `compass`      | HMC5883L, QMC5883L         | uint16 (deg)   |
| `temperature`  | DHT22, BME280              | int16 (0.1C)   |
| `light`        | BH1750, LDR                | uint16 (lux)   |
| `touch`        | capacitive pad, limit sw    | bool           |
| `encoder`      | rotary encoder, wheel tick  | int32 (count)  |

Each returns a `sig_data` with typed payload. The student doesn't
parse registers. They call `sense(distance)` and get millimeters.

---

## Motor Types (Zeros)

| Abstraction    | Hardware Examples           | Control Signal  |
|----------------|-----------------------------|-----------------|
| `dc_motor`     | L298N, TB6612                | speed + dir     |
| `servo`        | SG90, MG996R                | angle (0-180)   |
| `stepper`      | A4988, DRV8825             | steps + dir     |
| `continuous`   | continuous servo             | speed + dir     |

---

## For the Kernel Agent

The kernel needs:
1. `hw_driver` struct and `hw_register()` function in a new `hw.h`
2. I2C bus scanning (many sensors use I2C)
3. GPIO pin abstraction (motors, digital sensors)
4. Timer-based PWM output (motor speed control)
5. The `scan` command in shell.c already calls `pci_device_count()` —
   extend to also enumerate I2C and GPIO devices when those subsystems exist

---

## For the Z+ Agent

The language needs:
1. `sense()` and `act()` as built-in functions that route to signal nodes
2. Hardware discovery: `devices -> gate(type: motor)` returns connected motors
3. The `->` operator already connects signals — hardware IS signals
4. Hot-plug: when a device appears on the bus, its signal node appears in the graph
5. Templates should use the same API for both screen and hardware targets

---

## Priority Order

1. GPIO output (LED blink — proof of life for robotics)
2. PWM output (motor speed — proof of movement)
3. I2C read (sensor value — proof of sensing)
4. Full sense/act API
5. Game entity mapping (same API for sprites)
