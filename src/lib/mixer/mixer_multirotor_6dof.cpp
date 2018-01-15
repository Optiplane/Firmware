/****************************************************************************
 *
 *   Copyright (c) 2012-2016 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mixer_multirotor_6dof.cpp
 *
 * Multi-rotor mixers, 6 degrees of freedom
 *
 * @author Julien Lecoeur <julien.lecoeur@gmail.com>
 */

#include <px4_config.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <math.h>

#include <mathlib/math/Limits.hpp>
#include <drivers/drv_pwm_output.h>

#include "mixer.h"

// This file is generated by the px_generate_mixers.py script which is invoked during the build process
#include "mixer_multirotor_6dof_normalized.generated.h"

#define debug(fmt, args...)	do { } while(0)
//#define debug(fmt, args...)	do { printf("[mixer] " fmt "\n", ##args); } while(0)
//#include <debug.h>
//#define debug(fmt, args...)	syslog(fmt "\n", ##args)

/*
 * Clockwise: 1
 * Counter-clockwise: -1
 */

MultirotorMixer6dof::MultirotorMixer6dof(ControlCallback control_cb,
		uintptr_t cb_handle,
		MultirotorGeometry geometry,
		float roll_scale,
		float pitch_scale,
		float yaw_scale,
		float x_scale,
		float y_scale,
		float z_scale,
		float idle_speed) :
	Mixer(control_cb, cb_handle),
	_roll_scale(roll_scale),
	_pitch_scale(pitch_scale),
	_yaw_scale(yaw_scale),
	_x_scale(x_scale),
	_y_scale(y_scale),
	_z_scale(z_scale),
	_idle_speed(-1.0f + idle_speed * 2.0f),	/* shift to output range here to avoid runtime calculation */
	_out_max(1.0f),
	_out_min(idle_speed),
	_delta_out_max(0.0f),
	_thrust_factor(0.0f),
	_rotor_count(_config_rotor_count[(MultirotorGeometryUnderlyingType)geometry]),
	_rotors(_config_index[(MultirotorGeometryUnderlyingType)geometry]),
	_outputs_prev(new float[_rotor_count])
{
	memset(_outputs_prev, _idle_speed, _rotor_count * sizeof(float));

	// Check for uncontrolled axes
	for (size_t j = 0; j < 6; j++) {
		float norm2 = 0.0f;

		for (size_t i = 0; i < _rotor_count; i++) {
			norm2 += _rotors[i].scale[j] * _rotors[i].scale[j];
		}

		if (norm2 > 1e-6f) {
			_controlled_axes[j] = true;

		} else {
			_controlled_axes[j] = false;
		}
	}
}

MultirotorMixer6dof::~MultirotorMixer6dof()
{
	if (_outputs_prev != nullptr) {
		delete[] _outputs_prev;
	}
}

MultirotorMixer6dof *
MultirotorMixer6dof::from_text(Mixer::ControlCallback control_cb, uintptr_t cb_handle, const char *buf,
			       unsigned &buflen)
{
	MultirotorGeometry geometry = MultirotorGeometry::MAX_GEOMETRY;
	char geomname[8];
	int s[7];
	int used;

	/* enforce that the mixer ends with a new line */
	if (!string_well_formed(buf, buflen)) {
		return nullptr;
	}

	if (sscanf(buf, "S: %7s %d %d %d %d %d %d %d%n",
		   geomname, &s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &s[6], &used) != 8) {
		debug("multirotor parse failed on '%s'", buf);
		return nullptr;
	}

	if (used > (int)buflen) {
		debug("OVERFLOW: multirotor spec used %d of %u", used, buflen);
		return nullptr;
	}

	buf = skipline(buf, buflen);

	if (buf == nullptr) {
		debug("no line ending, line is incomplete");
		return nullptr;
	}

	debug("remaining in buf: %d, first char: %c", buflen, buf[0]);

	for (MultirotorGeometryUnderlyingType i = 0; i < (MultirotorGeometryUnderlyingType)MultirotorGeometry::MAX_GEOMETRY;
	     i++) {
		if (!strcmp(geomname, _config_key[i])) {
			geometry = (MultirotorGeometry)i;
			break;
		}
	}

	if (geometry == MultirotorGeometry::MAX_GEOMETRY) {
		debug("unrecognised geometry '%s'", geomname);
		return nullptr;
	}

	debug("adding multirotor mixer '%s'", geomname);

	return new MultirotorMixer6dof(
		       control_cb,
		       cb_handle,
		       geometry,
		       s[0] / 10000.0f,
		       s[1] / 10000.0f,
		       s[2] / 10000.0f,
		       s[3] / 10000.0f,
		       s[4] / 10000.0f,
		       s[5] / 10000.0f,
		       s[6] / 10000.0f);
}

matrix::Vector<float, 6>
MultirotorMixer6dof::get_command(void) const
{
	const float command_[6] = {
		math::constrain(get_control(0, actuator_controls_s::INDEX_ROLL) 	* _roll_scale,  -1.0f, 1.0f),
		math::constrain(get_control(0, actuator_controls_s::INDEX_PITCH) 	* _pitch_scale, -1.0f, 1.0f),
		math::constrain(get_control(0, actuator_controls_s::INDEX_YAW)	 	* _yaw_scale,   -1.0f, 1.0f),
		math::constrain(get_control(0, actuator_controls_s::INDEX_X_THRUST) * _x_scale, 	-1.0f, 1.0f),
		math::constrain(get_control(0, actuator_controls_s::INDEX_Y_THRUST) * _y_scale, 	-1.0f, 1.0f),
		math::constrain(get_control(0, actuator_controls_s::INDEX_Z_THRUST) * _z_scale, 	-1.0f, 1.0f),
	};

	// return command;
	return matrix::Vector<float, 6>(command_);
}


matrix::Vector<float, 6>
MultirotorMixer6dof::clip_command(const matrix::Vector<float, 6> &desired_command) const
{
	// Baseline command that does not saturate motors, [0 0 0 0 0 0] is always ok
	matrix::Vector<float, 6> command;

	// Try to copy certain elements of command into command to give them priority
	const bool prioritary_axes[][6] = {
		{false, false, false, false, false, true},	 // first try to copy Z thrust
		{true,  true,  false, false, false, false},  // then try to copy roll and pitch
		{false, false, true,  false, false, false},  // then try to copy yaw
		{false, false, false, true,  true,  false},  // then try to copy X and Y
	};

	for (size_t i = 0; i < sizeof(prioritary_axes) / sizeof(prioritary_axes[0]); i++) {
		// Copy prioritary axes into new command
		matrix::Vector<float, 6> new_command = command;

		for (size_t j = 0; j < 6; j++) {
			if (prioritary_axes[i][j]) {
				new_command(j) = desired_command(j);
			}
		}

		// Project desired command on unsaturated convex by moving it towards command which is inside the convex
		// The vector u is used to drive the command towards the non-saturated zone when the command saturates at least one motor
		// command = command + k * u
		// where k is a scalar equal 1 when there is no saturation
		matrix::Vector<float, 6> u = new_command - command;
		bool saturation = false;

		for (size_t ii = 0; ii < _rotor_count; ii++) {
			// rotor scale, b is the vector normal to the saturation planes
			const matrix::Vector<float, 6> b(_rotors[ii].scale);

			// compute motor command to check if the motor is saturated
			float out = new_command * b;

			// if motor is saturated
			// bring command closer to command in order to un-saturate this motor
			if (out > _out_max) {
				saturation = true;
				float ub = u * b;

				if (fabsf(ub) > 1e-6f) {
					float k = (_out_max - command * b) / ub;

					if ((k >= 0.0f) && (k <= 1.0f)) {
						// new_command = command + 0.8f * k * u; // the factor 0.8 is to keep a margin to saturation
						new_command = command + k * u;
					}
				}

			} else if (out < _out_min) {
				saturation = true;
				float ub = u * b;

				if (fabsf(ub) > 1e-6f) {
					float k = (_out_min - command * b) / ub;

					if ((k >= 0.0f) && (k <= 1.0f)) {
						// new_command = command + 0.8f * k * u; // the factor 0.8 is to keep a margin to saturation
						new_command = command + k * u;
					}
				}
			}
		}

		// New baseline is now in unsaturated domain, use that as baseline
		command = new_command;

		if (saturation) {
			// We failed at copying this prioritized axis
		}
	}

	return command;
}


unsigned
MultirotorMixer6dof::mix(float *outputs, unsigned space)
{
	/* Summary of mixing strategy:
	The command is represented as a vector y of dimension 6 (roll pitch yaw x y z).
	Scale factors for rotor i are represented as a vector b_i of dimension 6.
	Each rotor vector is normal to 2 planes defined as (y . b_i) = 1 (high motor saturation) and (y . b_i) = 0 (low motor saturation),
	The command vector y should be between these two planes so that motor i is not saturated.
	Baseline command y_s is a command that does not saturate any motor.
	1) for each rotor:
	   a) mix roll, pitch yaw and thrust (out_i = y . b_i)
	   b) if the output violate range [0,1] then shift the command towards the baseline command
	      so that the new command is on one of the two saturation planes for this motor.
	3) recompute all outputs with new command
	4) scale all outputs to range [idle_speed,1]
	*/

	// Get raw command
	const matrix::Vector<float, 6> raw_command = get_command();

	// Remove uncontrolled axes
	matrix::Vector<float, 6> command = raw_command;

	for (size_t j = 0; j < 6; j++) {
		if (not _controlled_axes[j]) {
			command(j) = 0.0f;
		}
	}

	// Make sure the command is in the feaseable actuation space
	command = clip_command(command);

	// Compute mixing
	for (unsigned i = 0; i < _rotor_count; i++) {
		// rotor scale, b is the vector normal to the saturation planes
		const matrix::Vector<float, 6> b(_rotors[i].scale);

		// motor command
		outputs[i] = command * b;

		/*
			implement simple model for static relationship between applied motor pwm and motor thrust
			model: thrust = (1 - _thrust_factor) * PWM + _thrust_factor * PWM^2
			this model assumes normalized input / output in the range [0,1] so this is the right place
			to do it as at this stage the outputs are in that range.
		 */
		if (_thrust_factor > 0.0f) {
			outputs[i] = -(1.0f - _thrust_factor) / (2.0f * _thrust_factor) + sqrtf((1.0f - _thrust_factor) *
					(1.0f - _thrust_factor) / (4.0f * _thrust_factor * _thrust_factor) + (outputs[i] < 0.0f ? 0.0f : outputs[i] /
							_thrust_factor));
		}

		// scale output to range [_out_min, _out_max]
		outputs[i] = math::constrain(outputs[i], _out_min, _out_max);

		// scale output to range [-1, 1]
		// outputs[i] = math::constrain(-1.0f + 2.0f * outputs[i], _idle_speed, _out_max);
	}

	// clean out class variable used to capture saturation
	_saturation_status.value = 0;

	// Update saturation based on controlled axes
	_saturation_status.flags.x_thrust_valid = _controlled_axes[3];
	_saturation_status.flags.y_thrust_valid = _controlled_axes[4];
	_saturation_status.flags.z_thrust_valid = _controlled_axes[5];

	// slew rate limiting and saturation checking
	for (unsigned i = 0; i < _rotor_count; i++) {
		bool clipping_high = false;
		bool clipping_low = false;

		// check for saturation against static limits
		if (outputs[i] > 0.99f) {
			clipping_high = true;

		} else if (outputs[i] < _out_min + 0.01f) {
			clipping_low = true;

		}

		// check for saturation against slew rate limits
		if (_delta_out_max > 0.0f) {
			float delta_out = outputs[i] - _outputs_prev[i];

			if (delta_out > _delta_out_max) {
				outputs[i] = _outputs_prev[i] + _delta_out_max;
				clipping_high = true;

			} else if (delta_out < -_delta_out_max) {
				outputs[i] = _outputs_prev[i] - _delta_out_max;
				clipping_low = true;

			}
		}

		_outputs_prev[i] = outputs[i];

		// update the saturation status report
		update_saturation_status(i, clipping_high, clipping_low);
	}

	// this will force the caller of the mixer to always supply new slew rate values, otherwise no slew rate limiting will happen
	_delta_out_max = 0.0f;

	return _rotor_count;
}

/*
 * This function update the control saturation status report using the following inputs:
 *
 * index: 0 based index identifying the motor that is saturating
 * clipping_high: true if the motor demand is being limited in the positive direction
 * clipping_low: true if the motor demand is being limited in the negative direction
*/
void
MultirotorMixer6dof::update_saturation_status(unsigned index, bool clipping_high, bool clipping_low)
{
	// The motor is saturated at the upper limit
	// check which control axes and which directions are contributing
	if (clipping_high) {
		if (_rotors[index].scale[ROLL_COMMAND] > 0.0f) {
			// A positive change in roll will increase saturation
			_saturation_status.flags.roll_pos = true;

		} else if (_rotors[index].scale[ROLL_COMMAND] < 0.0f) {
			// A negative change in roll will increase saturation
			_saturation_status.flags.roll_neg = true;
		}

		// check if the pitch input is saturating
		if (_rotors[index].scale[PITCH_COMMAND] > 0.0f) {
			// A positive change in pitch will increase saturation
			_saturation_status.flags.pitch_pos = true;

		} else if (_rotors[index].scale[PITCH_COMMAND] < 0.0f) {
			// A negative change in pitch will increase saturation
			_saturation_status.flags.pitch_neg = true;
		}

		// check if the yaw input is saturating
		if (_rotors[index].scale[YAW_COMMAND] > 0.0f) {
			// A positive change in yaw will increase saturation
			_saturation_status.flags.yaw_pos = true;

		} else if (_rotors[index].scale[YAW_COMMAND] < 0.0f) {
			// A negative change in yaw will increase saturation
			_saturation_status.flags.yaw_neg = true;
		}

		// check if the x input is saturating
		if (_rotors[index].scale[X_COMMAND] > 0.0f) {
			// A positive change in x will increase saturation
			_saturation_status.flags.x_thrust_pos = true;

		} else if (_rotors[index].scale[X_COMMAND] < 0.0f) {
			// A negative change in x will increase saturation
			_saturation_status.flags.x_thrust_neg = true;
		}

		// check if the y input is saturating
		if (_rotors[index].scale[Y_COMMAND] > 0.0f) {
			// A positive change in y will increase saturation
			_saturation_status.flags.y_thrust_pos = true;

		} else if (_rotors[index].scale[Y_COMMAND] < 0.0f) {
			// A negative change in y will increase saturation
			_saturation_status.flags.y_thrust_neg = true;
		}

		// check if the z input is saturating
		if (_rotors[index].scale[Z_COMMAND] > 0.0f) {
			// A positive change in z will increase saturation
			_saturation_status.flags.z_thrust_pos = true;

		} else if (_rotors[index].scale[Z_COMMAND] < 0.0f) {
			// A negative change in z will increase saturation
			_saturation_status.flags.z_thrust_neg = true;
		}
	}

	// The motor is saturated at the lower limit
	// check which control axes and which directions are contributing
	if (clipping_low) {
		// check if the roll input is saturating
		if (_rotors[index].scale[ROLL_COMMAND] > 0.0f) {
			// A negative change in roll will increase saturation
			_saturation_status.flags.roll_neg = true;

		} else if (_rotors[index].scale[ROLL_COMMAND] < 0.0f) {
			// A positive change in roll will increase saturation
			_saturation_status.flags.roll_pos = true;
		}

		// check if the pitch input is saturating
		if (_rotors[index].scale[PITCH_COMMAND] > 0.0f) {
			// A negative change in pitch will increase saturation
			_saturation_status.flags.pitch_neg = true;

		} else if (_rotors[index].scale[PITCH_COMMAND] < 0.0f) {
			// A positive change in pitch will increase saturation
			_saturation_status.flags.pitch_pos = true;
		}

		// check if the yaw input is saturating
		if (_rotors[index].scale[YAW_COMMAND] > 0.0f) {
			// A negative change in yaw will increase saturation
			_saturation_status.flags.yaw_neg = true;

		} else if (_rotors[index].scale[YAW_COMMAND] < 0.0f) {
			// A positive change in yaw will increase saturation
			_saturation_status.flags.yaw_pos = true;
		}

		// check if the x input is saturating
		if (_rotors[index].scale[X_COMMAND] > 0.0f) {
			// A negative change in x will increase saturation
			_saturation_status.flags.x_thrust_neg = true;

		} else if (_rotors[index].scale[X_COMMAND] < 0.0f) {
			// A positive change in x will increase saturation
			_saturation_status.flags.x_thrust_pos = true;
		}

		// check if the y input is saturating
		if (_rotors[index].scale[Y_COMMAND] > 0.0f) {
			// A negative change in y will increase saturation
			_saturation_status.flags.y_thrust_neg = true;

		} else if (_rotors[index].scale[Y_COMMAND] < 0.0f) {
			// A positive change in y will increase saturation
			_saturation_status.flags.y_thrust_pos = true;
		}

		// check if the z input is saturating
		if (_rotors[index].scale[Z_COMMAND] > 0.0f) {
			// A negative change in z will increase saturation
			_saturation_status.flags.z_thrust_neg = true;

		} else if (_rotors[index].scale[Z_COMMAND] < 0.0f) {
			// A positive change in z will increase saturation
			_saturation_status.flags.z_thrust_pos = true;
		}

	}

	_saturation_status.flags.valid = true;
}

void
MultirotorMixer6dof::groups_required(uint32_t &groups)
{
	/* XXX for now, hardcoded to indexes 0-3 in control group zero */
	groups |= (1 << 0);
}

uint16_t MultirotorMixer6dof::get_saturation_status()
{
	return _saturation_status.value;
}