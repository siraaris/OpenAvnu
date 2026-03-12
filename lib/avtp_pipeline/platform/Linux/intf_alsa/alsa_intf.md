ALSA interface {#alsa_intf}
==============

# Description

ALSA interface module. An interface to connect AVTP streams to ALSA either as an audio source or sink.

<br>
# Interface module configuration parameters

Name                      | Description
--------------------------|---------------------------
intf_nv_ignore_timestamp  | If set to 1 timestamps will be ignored during processing of frames. This also means stale (old) Media Queue items will not be purged.
intf_nv_device_name       | ALSA device name. Commonly "default" or "plug:dmix"
intf_nv_audio_rate        | Audio rate, numberic values defined by @ref avb_audio_rate_t
intf_nv_audio_bit_depth   | Bit depth of audio, numeric values defined by @ref avb_audio_bit_depth_t
intf_nv_audio_type        | Type of data samples, possible values <ul><li>float</li><li>sign</li><li>unsign</li><li>int</li><li>uint</li></ul>
intf_nv_audio_endian      | Data endianess possible values <ul><li>big</li><li>little</li></ul>
intf_nv_audio_channels    | Number of audio channels, numeric values should be within range of values in @ref avb_audio_channels_t
intf_nv_allow_resampling  | If 1 software resampling allowed, disallowed otherwise (by default allowed)
intf_nv_start_threshold_periods | Playback start threshold measured in ALSA periods (2 by default)
intf_nv_period_time       | Approximate ALSA period duration in microseconds
intf_nv_clock_skew_ppb    | Estimate of media clock skew in Parts Per Billion (nanoseconds per second)
intf_nv_drift_log_interval_sec | If >0, logs measured drift between ALSA frame progression and AVTP timestamp progression every N seconds (talker side)
intf_nv_drift_comp_enable | If 1, enables adaptive fixed-timestamp skew compensation from measured drift
intf_nv_drift_comp_gain | Compensation loop gain (typical 0.1 to 0.5)
intf_nv_drift_comp_max_step_skew_ppb | Max skew adjustment per update interval
intf_nv_drift_comp_min_skew_ppb | Minimum allowed skew value
intf_nv_drift_comp_max_skew_ppb | Maximum allowed skew value

<br>
# Notes

There are some parameters that have to be set during configuration of this
interface module and before configuring mapping:
* [AAF audio mapping](@ref aaf_audio_map)
* [Uncompressed audio mapping](@ref uncmp_audio_map)

These parameters can be set in either:
* in the ini file (or available configuration system), so values will be parsed 
in the intf_cfg_cb function, 
* in the interface module initialization function where valid values can be 
assigned directly 

Values assigned in the intf_cfg_cb function will override any values set in the 
initialization function. 
