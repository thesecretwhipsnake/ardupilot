#include "Copter.h"

// return barometric altitude in centimeters
void Copter::read_barometer(void)
{
    barometer.update();

    baro_alt = barometer.get_altitude() * 100.0f;
}

void Copter::init_rangefinder(void)
{
#if RANGEFINDER_ENABLED == ENABLED
   rangefinder.set_log_rfnd_bit(MASK_LOG_CTUN);
   rangefinder.init(ROTATION_PITCH_270);
   rangefinder_state.alt_cm_filt.set_cutoff_frequency(g2.rangefinder_filt);
   rangefinder_state.enabled = rangefinder.has_orientation(ROTATION_PITCH_270);

   // upward facing range finder
   rangefinder_up_state.alt_cm_filt.set_cutoff_frequency(g2.rangefinder_filt);
   rangefinder_up_state.enabled = rangefinder.has_orientation(ROTATION_PITCH_90);
#endif
}

// return rangefinder altitude in centimeters
void Copter::read_rangefinder(void)
{
#if RANGEFINDER_ENABLED == ENABLED
    rangefinder.update();

#if RANGEFINDER_TILT_CORRECTION == ENABLED
    const float tilt_correction = MAX(0.707f, ahrs.get_rotation_body_to_ned().c.z);
#else
    const float tilt_correction = 1.0f;
#endif

    // iterate through downward and upward facing lidar
    struct {
        RangeFinderState &state;
        enum Rotation orientation;
    } rngfnd[2] = {{rangefinder_state, ROTATION_PITCH_270}, {rangefinder_up_state, ROTATION_PITCH_90}};

    for (uint8_t i=0; i < ARRAY_SIZE(rngfnd); i++) {
        // local variables to make accessing simpler
        RangeFinderState &rf_state = rngfnd[i].state;
        enum Rotation rf_orient = rngfnd[i].orientation;

        // update health
        rf_state.alt_healthy = ((rangefinder.status_orient(rf_orient) == RangeFinder::Status::Good) &&
                                (rangefinder.range_valid_count_orient(rf_orient) >= RANGEFINDER_HEALTH_MAX));

        // tilt corrected but unfiltered, not glitch protected alt
        rf_state.alt_cm = tilt_correction * rangefinder.distance_cm_orient(rf_orient);

        // remember inertial alt to allow us to interpolate rangefinder
        rf_state.inertial_alt_cm = inertial_nav.get_position_z_up_cm();

        // glitch handling.  rangefinder readings more than RANGEFINDER_GLITCH_ALT_CM from the last good reading
        // are considered a glitch and glitch_count becomes non-zero
        // glitches clear after RANGEFINDER_GLITCH_NUM_SAMPLES samples in a row.
        // glitch_cleared_ms is set so surface tracking (or other consumers) can trigger a target reset
        const int32_t glitch_cm = rf_state.alt_cm - rf_state.alt_cm_glitch_protected;
        bool reset_terrain_offset = false;
        if (glitch_cm >= RANGEFINDER_GLITCH_ALT_CM) {
            rf_state.glitch_count = MAX(rf_state.glitch_count+1, 1);
        } else if (glitch_cm <= -RANGEFINDER_GLITCH_ALT_CM) {
            rf_state.glitch_count = MIN(rf_state.glitch_count-1, -1);
        } else {
            rf_state.glitch_count = 0;
            rf_state.alt_cm_glitch_protected = rf_state.alt_cm;
        }
        if (abs(rf_state.glitch_count) >= RANGEFINDER_GLITCH_NUM_SAMPLES) {
            // clear glitch and record time so consumers (i.e. surface tracking) can reset their target altitudes
            rf_state.glitch_count = 0;
            rf_state.alt_cm_glitch_protected = rf_state.alt_cm;
            rf_state.glitch_cleared_ms = AP_HAL::millis();
            reset_terrain_offset = true;
        }

        // filter rangefinder altitude
        uint32_t now = AP_HAL::millis();
        const bool timed_out = now - rf_state.last_healthy_ms > RANGEFINDER_TIMEOUT_MS;
        if (rf_state.alt_healthy) {
            if (timed_out) {
                // reset filter if we haven't used it within the last second
                rf_state.alt_cm_filt.reset(rf_state.alt_cm);
                reset_terrain_offset = true;

            } else {
                rf_state.alt_cm_filt.apply(rf_state.alt_cm, 0.05f);
            }
            rf_state.last_healthy_ms = now;
        }

        // handle reset of terrain offset
        if (reset_terrain_offset) {
            if (rf_orient == ROTATION_PITCH_90) {
                // upward facing
                rf_state.terrain_offset_cm = rf_state.inertial_alt_cm + rf_state.alt_cm;
            } else {
                // assume downward facing
                rf_state.terrain_offset_cm = rf_state.inertial_alt_cm - rf_state.alt_cm;
            }
        }

        // send downward facing lidar altitude and health to the libraries that require it
#if HAL_PROXIMITY_ENABLED
        if (rf_orient == ROTATION_PITCH_270) {
            if (rangefinder_state.alt_healthy || timed_out) {
                g2.proximity.set_rangefinder_alt(rangefinder_state.enabled, rangefinder_state.alt_healthy, rangefinder_state.alt_cm_filt.get());
            }
        }
#endif
    }

#else
    // downward facing rangefinder
    rangefinder_state.enabled = false;
    rangefinder_state.alt_healthy = false;
    rangefinder_state.alt_cm = 0;

    // upward facing rangefinder
    rangefinder_up_state.enabled = false;
    rangefinder_up_state.alt_healthy = false;
    rangefinder_up_state.alt_cm = 0;
#endif
}

// return true if rangefinder_alt can be used
bool Copter::rangefinder_alt_ok() const
{
    return (rangefinder_state.enabled && rangefinder_state.alt_healthy);
}

// return true if rangefinder_alt can be used
bool Copter::rangefinder_up_ok() const
{
    return (rangefinder_up_state.enabled && rangefinder_up_state.alt_healthy);
}

// update rangefinder based terrain offset
// terrain offset is the terrain's height above the EKF origin
void Copter::update_rangefinder_terrain_offset()
{
    float terrain_offset_cm = rangefinder_state.inertial_alt_cm - rangefinder_state.alt_cm_glitch_protected;
    rangefinder_state.terrain_offset_cm += (terrain_offset_cm - rangefinder_state.terrain_offset_cm) * (copter.G_Dt / MAX(copter.g2.surftrak_tc, copter.G_Dt));

    terrain_offset_cm = rangefinder_up_state.inertial_alt_cm + rangefinder_up_state.alt_cm_glitch_protected;
    rangefinder_up_state.terrain_offset_cm += (terrain_offset_cm - rangefinder_up_state.terrain_offset_cm) * (copter.G_Dt / MAX(copter.g2.surftrak_tc, copter.G_Dt));

    if (rangefinder_state.alt_healthy || (AP_HAL::millis() - rangefinder_state.last_healthy_ms > RANGEFINDER_TIMEOUT_MS)) {
        wp_nav->set_rangefinder_terrain_offset(rangefinder_state.enabled, rangefinder_state.alt_healthy, rangefinder_state.terrain_offset_cm);
#if MODE_CIRCLE_ENABLED
        circle_nav->set_rangefinder_terrain_offset(rangefinder_state.enabled && wp_nav->rangefinder_used(), rangefinder_state.alt_healthy, rangefinder_state.terrain_offset_cm);
#endif
    }
}

/*
  get inertially interpolated rangefinder height. Inertial height is
  recorded whenever we update the rangefinder height, then we use the
  difference between the inertial height at that time and the current
  inertial height to give us interpolation of height from rangefinder
 */
bool Copter::get_rangefinder_height_interpolated_cm(int32_t& ret) const
{
    if (!rangefinder_alt_ok()) {
        return false;
    }
    ret = rangefinder_state.alt_cm_filt.get();
    float inertial_alt_cm = inertial_nav.get_position_z_up_cm();
    ret += inertial_alt_cm - rangefinder_state.inertial_alt_cm;
    return true;
}

// update wheel encoders
void Copter::update_wheel_encoder()
{
    // exit immediately if not enabled
    if (g2.wheel_encoder.num_sensors() == 0) {
        return;
    }

    // update encoders
    g2.wheel_encoder.update();

    // save cumulative distances at current time (in meters) for reporting to GCS
    for (uint8_t i = 0; i < g2.wheel_encoder.num_sensors(); i++) {
        wheel_encoder_last_distance_m[i] = g2.wheel_encoder.get_distance(i);
    }

    // send wheel encoder delta angle and delta time to EKF
    // this should not be done at more than 50hz
    // initialise on first iteration
    if (!wheel_encoder_initialised) {
        wheel_encoder_initialised = true;
        for (uint8_t i = 0; i < g2.wheel_encoder.num_sensors(); i++) {
            wheel_encoder_last_angle_rad[i] = g2.wheel_encoder.get_delta_angle(i);
            wheel_encoder_last_reading_ms[i] = g2.wheel_encoder.get_last_reading_ms(i);
        }
        return;
    }

    // on each iteration send data from alternative wheel encoders
    wheel_encoder_last_index_sent++;
    if (wheel_encoder_last_index_sent >= g2.wheel_encoder.num_sensors()) {
        wheel_encoder_last_index_sent = 0;
    }

    // get current time, total delta angle (since startup) and update time from sensor
    const float curr_angle_rad = g2.wheel_encoder.get_delta_angle(wheel_encoder_last_index_sent);
    const uint32_t sensor_reading_ms = g2.wheel_encoder.get_last_reading_ms(wheel_encoder_last_index_sent);
    const uint32_t now_ms = AP_HAL::millis();

    // calculate angular change (in radians)
#if HAL_NAVEKF3_AVAILABLE
    const float delta_angle = curr_angle_rad - wheel_encoder_last_angle_rad[wheel_encoder_last_index_sent];
#endif
    wheel_encoder_last_angle_rad[wheel_encoder_last_index_sent] = curr_angle_rad;

    // calculate delta time using time between sensor readings or time since last send to ekf (whichever is shorter)
    uint32_t sensor_diff_ms = sensor_reading_ms - wheel_encoder_last_reading_ms[wheel_encoder_last_index_sent];
    if (sensor_diff_ms == 0 || sensor_diff_ms > 100) {
        // if no sensor update or time difference between sensor readings is too long use time since last send to ekf
        sensor_diff_ms = now_ms - wheel_encoder_last_reading_ms[wheel_encoder_last_index_sent];
        wheel_encoder_last_reading_ms[wheel_encoder_last_index_sent] = now_ms;
    } else {
        wheel_encoder_last_reading_ms[wheel_encoder_last_index_sent] = sensor_reading_ms;
    }
#if HAL_NAVEKF3_AVAILABLE
    const float delta_time = sensor_diff_ms * 0.001f;

    /* delAng is the measured change in angular position from the previous measurement where a positive rotation is produced by forward motion of the vehicle (rad)
     * delTime is the time interval for the measurement of delAng (sec)
     * timeStamp_ms is the time when the rotation was last measured (msec)
     * posOffset is the XYZ body frame position of the wheel hub (m)
     */
    ahrs.EKF3.writeWheelOdom(delta_angle,
                        delta_time,
                        wheel_encoder_last_reading_ms[wheel_encoder_last_index_sent],
                        g2.wheel_encoder.get_pos_offset(wheel_encoder_last_index_sent),
                        g2.wheel_encoder.get_wheel_radius(wheel_encoder_last_index_sent));
#endif
}