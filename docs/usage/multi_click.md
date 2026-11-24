# Momentary switch gestures

Gestures apply to momentary and normally closed momentary switches. A toggle
switch still reports its physical position. Gesture settings are per switch and
survive a restart. The existing relay/switch settings retain their NVM layout.

On the AVATTO LZWSM16-1 use the updated `switch_custom.js` converter in
Zigbee2MQTT. Ensure switch endpoint 1 (`switch`) has its `genMultistateInput`
binding to the coordinator and attribute reporting configured (the converter's
Configure action sets these up). Use the `action` event, not the diagnostic
`switch_press_action_switch` state, to drive automations:

- `switch_single`, `switch_double`, `switch_triple`
- `switch_single_hold` / `switch_single_release`, and equivalent double/triple
  variants when the last press of a sequence is held

The converter only publishes `action` from explicit reports of attribute
`0xff00` in `genMultistateInput` (readback is always zero). Periodic
`presentValue` reports and read responses do not generate action events.
Use firmware and converter from the same revision; older builds still expose
their diagnostic state but do not emit events on the new attribute.

Set `max_press_count_switch` to **2** for double clicks or **3** to include
triple clicks. The firmware default is **1**, which reports a single click on
debounced release without waiting. With a limit above 1, the switch waits
`confirm_release_ms_switch` after each short release, even at the maximum count
(extra clicks in the same sequence saturate at the limit). The default window
is **200 ms**. It measures debounced events, not the physical edge times. A
hold uses the existing long-press threshold. The native configuration attributes
in `genOnOffSwitchCfg` are `0xff07` (uint8, 1–3) and `0xff08` (uint16, 1–2000 ms).
Writing zero restores the default for that setting.
Unchanged settings do not write flash. When a save fails, readback returns the
previous settings; a successful ZCL write response alone does not prove
persistence. If the Telink timer reserve would be used, the pending gesture
is dropped. In `single_click` binding mode no On/Off command is sent for a
dropped gesture; legacy edge-triggered binding and local relay behavior continue.

## Confirmed single-click binding (phase 2)

The default, `short_press`, is unchanged: it sends an On/Off command on each
short release even during a multi-click gesture. To send only on confirmed
single clicks, choose `single_click` in `switch_binded_mode_switch`. It uses the
configured On/Off action (`toggle_simple` sends Toggle). When `max_press_count`
is 1 there is no extra wait after release; for 2 or 3 it waits the configured
confirmation window. Double/triple clicks and holds send no On/Off command in
this mode. Local relay modes, physical toggle switches, gesture events and
Move/StopWithOnOff are unaffected.

`single_click` adds enum value `0x04` to `genOnOffSwitchCfg` attribute `0xff05`.
The Zigbee2MQTT converter lists the option but will not write it unless a live
read of the selected endpoint confirms support (read-only attribute `0xff09`).
The legacy values and NVM layout are unchanged. Before downgrading, set the
binding mode back to `short_press` (`0x03`) and read it back: older firmware
does not recognize the new mode and will not send its momentary On/Off commands
while it is selected.

For smart sync/opposite, the relay state used for the command is taken on short
release, after the local relay action, not at the end of the confirmation window.
If the effective binding settings change during confirmation, the firmware drops
the pending command without suppressing an otherwise valid gesture report.

Known inherited limitation: contact bounce close to the hold threshold can
classify a hold as a short press. This affects both `short_press` and
`single_click`; Phase 2 does not change the button debounce logic.

## Native scene recall (phase 3)

Supported Telink switches advertise a `genScenes` **output** cluster. The
switch does not store scenes: the lamps in a Zigbee group do. After loading
the matching external converter and updating the switch firmware:

1. Run Interview and Reconfigure on the switch. In Zigbee2MQTT, bind the
   switch endpoint's `genScenes` cluster to **exactly one** Zigbee group. Do
   not bind Scenes to the coordinator or directly to a device. Keep the
   existing On/Off, Level Control and gesture reporting bindings.
2. In the same Zigbee group, store the two scenes (`scene_store`) and verify
   them with `scene_recall` in Zigbee2MQTT. For example, send
   `{"scene_recall":1}` to `zigbee2mqtt/<group>/set`. Use known stable
   brightness values on a no-neutral installation.
3. On the switch's Exposes page, select `single_click` binding mode and set
   the click limit to `3` for both double and triple. Set `scene_group_id_switch`
   to the numeric Zigbee group ID, set `double_click_scene_id_switch` and
   `triple_click_scene_id_switch` (defaults **1** and **2**), then set
   `scene_enabled_switch` to true. Reads after writes confirm the applied value.

Scene recall is disabled by default. A missing, mismatched, ambiguous or
unicast Scenes binding causes no scene command to be sent. The destination
group must match the Group ID in the RecallScene payload. Group ID `0` means
unset; disable recall before clearing it. Scene IDs `0`–`255` are accepted.
By default, single clicks still send only the configured On/Off action. See
below for optional recall alongside Toggle. The hold/release and Zigbee2MQTT
actions are unchanged. Direct group recall does
not require HA or Zigbee2MQTT to run after configuration, but the Zigbee mesh
and the End Device's parent must remain available.

### Optional single-click scene alongside Toggle

With Scenes enabled, binding mode `single_click` and action `toggle_simple`,
set `single_click_scene_id_switch` (default **0**) and turn on
`single_click_scene_enabled_switch` (off by default). On a confirmed single
click the switch submits Toggle first, then recalls the selected scene in the
same Scenes group. Keep the On/Off binding pointed at the same lamps. A missing
or invalid Scenes binding does not stop Toggle.

**Warning:** create this scene with `scene_add` and **omit `state`**. Do not
use a scene that stores `ON` or `OFF`, including one captured by `scene_store`
that contains On/Off. Its recall can override Toggle and prevent the light from
staying off or on. For example, where the target group supports both attributes:

```json
{"scene_add":{"ID":0,"brightness":140,"color_temp":370}}
```

Test both directions on the actual lamp. Toggle and RecallScene are separate
Zigbee transmissions, not an atomic operation. Lamp firmware may ignore changes
while off or apply the scene differently; some lamps may turn on even when
`state` was omitted. Verify that a click from on actually leaves the lamp off,
and disable this option if it does not. This does not change `color_temp_startup`.

Silicon Labs builds and layouts without sufficient descriptor capacity do not
advertise this feature. The new per-switch scene record does not change the
legacy relay or gesture NVM records. Earlier Phase 3 Scenes settings are read
with single-click recall disabled; after a Scenes settings write the old
Phase 3 build cannot read the new record. Group 0/unicast scenes and multiple group targets are not
supported. No default color-temperature logic is applied to Toggle.

## Verification without hardware

- Run `make tests` for the router/End Device stubs, including gesture timing,
  per-endpoint settings, legacy NVM, and AVATTO detached binding tests for both
  `short_press` and `single_click`.
- From the repository root, run `node tests/test_switch_gesture_converter.cjs`
  with `zigbee-herdsman-converters` 26.125.0 and `zigbee-herdsman` 11.2.0 on the
  Node module path (Zigbee2MQTT 2.14.3 dependencies).
- With the Telink SDK/toolchain and `yq` installed, run
  `make -f board.mk build-firmware BOARD=MODULE_AVATTO_TS0011 DEVICE_TYPE=end_device BIN_PATH=build/phase2-avatto`.
  This builds the binary without publishing an OTA or modifying the OTA index.

These checks do not validate radio delivery, missed GPIO interrupts, or rollback
on an installed module. A controlled OTA test requires a separate decision.
The Router and End Device releases for this AVATTO model share OTA identifiers.
Do not install an End Device test index globally. Verify the intended module's
IEEE address and `type: EndDevice` in Zigbee2MQTT (or in `bridge/devices`) and
use only an explicitly targeted check with the test index. An OTA match does
not prove the role. A Router-to-End-Device flash clears network settings.
`helper_scripts/check_avatto_end_device.py` accepts a current `bridge/devices`
JSON export and the target IEEE address, and prints only the payload for
`bridge/request/device/ota_update/check`; it does not connect to MQTT.
Zigbee2MQTT 1.42.0 is not covered: its converters library lacks the `text`
modern extend that the upstream legacy converter already required.
