from pathlib import Path

import pytest

from client import StubProc
from tests.conftest import Device
from tests.test_switch_gestures import collect_reports, setup_button
from tests.zcl_consts import (
    ZCL_CLUSTER_LEVEL_CONTROL,
    ZCL_CLUSTER_ON_OFF,
    ZCL_CLUSTER_ON_OFF_SWITCH_CONFIG as SWITCH_CFG,
    ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF,
    ZCL_CMD_LEVEL_STOP_WITH_ON_OFF,
    ZCL_CMD_ONOFF_ON,
    ZCL_CMD_ONOFF_TOGGLE,
    ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT,
    ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SINGLE_CLICK,
    ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED,
    ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF,
)

SCENES = 0x0005
ENABLED, GROUP, DOUBLE, TRIPLE = range(0xFF0A, 0xFF0E)
SINGLE_ENABLED, SINGLE = 0xFF0E, 0xFF0F
AVATTO = "hbxsdd6k;TS0011-avatto;BB4u;LB5;SC0u;RC2;"


@pytest.fixture
def device_config():
    return "StubManufacturer;StubScenes;LC0;SA0u;SA1u;RB0;RB1;"


def bind(device, group_id=0x1234, ep=1, cluster=SCENES, kind="group"):
    assert device.p.exec(f"bind {ep} {cluster:x} {group_id:x} {kind}").ok


def expect_descriptor(proc, endpoint, expected, cluster=SCENES):
    # Query the actual endpoint descriptor instead of inferring it from sends.
    result = proc.exec(f"zcl_dir {endpoint} {cluster:x}")
    if expected is None:
        assert not result.ok
    else:
        assert result.ok and result.payload["server"] == str(expected)


def set_scenes(device, enabled=1, mode=ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SINGLE_CLICK):
    setup_button(device, 1, 3)
    device.zcl_switch_relay_mode_set(1, ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED)
    device.zcl_switch_binding_mode_set(1, mode)
    device.write_zigbee_attr(1, SWITCH_CFG, GROUP, 0x1234)
    device.write_zigbee_attr(1, SWITCH_CFG, ENABLED, enabled)


def click_sequence(device, pin, count):
    for _ in range(count):
        device.click_button(pin)
    device.zcl_switch_get_multistate_value(1)


def finish_sequence(device):
    device.step_time(200)
    device.zcl_switch_get_multistate_value(1)


@pytest.mark.parametrize("count,gesture,scene", [(1, 5, None), (2, 7, 1), (3, 10, 2)])
def test_avatto_end_device_recall_wire_format(count, gesture, scene):
    with StubProc(cmd=["./build/stub/stub_end_device"], device_config=AVATTO) as proc:
        device = Device(proc)
        expect_descriptor(proc, 1, 0)
        expect_descriptor(proc, 2, None)
        device.set_gpio("C0", 1)
        device.step_time(60)
        set_scenes(device)
        bind(device)
        reports = collect_reports(proc)
        scene_frames = []
        proc.on_event.append(lambda ev: scene_frames.append(ev.payload) if
                             ev.kind == "zcl_cmd_send" and ev.payload["cluster"] == "0x0005" else None)
        click_sequence(device, "C0", count)
        device.step_time(199)
        assert device.zcl_relay_get(2) == "0"
        assert device.zcl_list_cmds(1) == [] and reports == []
        device.step_time(1)
        assert device.zcl_relay_get(2) == "0"
        assert reports == [(1, gesture)]
        if scene is None:
            assert [cmd.cmd for cmd in device.zcl_list_cmds(1, ZCL_CLUSTER_ON_OFF)] == [ZCL_CMD_ONOFF_TOGGLE]
            assert scene_frames == []
        else:
            assert device.zcl_list_cmds(1, ZCL_CLUSTER_ON_OFF) == []
            assert scene_frames == [{
                "ep": "1", "cluster": "0x0005", "cmd": "0x05", "len": "5",
                "data_hex": f"3412{scene:02X}FFFF", "group": "0x1234", "profile": "0x0104",
                "specific": "1", "direction": "0", "no_default_response": "1", "manufacturer": "0x0000",
            }]
        assert len(device.zcl_list_cmds(1)) == 1
        device.step_time(500)
        device.zcl_switch_get_multistate_value(1)
        assert len(device.zcl_list_cmds(1)) == 1


@pytest.mark.parametrize("target", ["missing", "other_endpoint", "other_cluster", "wrong_group", "unicast", "two_groups", "duplicate", "mixed", "disabled", "legacy_short"])
def test_scene_recall_skips_nonmatching_target_without_losing_action(device, button_pin, target):
    mode = (ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SHORT if target == "legacy_short"
            else ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SINGLE_CLICK)
    set_scenes(device, enabled=int(target != "disabled"), mode=mode)
    if target == "other_endpoint":
        bind(device, ep=2)
    if target == "other_cluster":
        bind(device, cluster=ZCL_CLUSTER_ON_OFF)
    if target == "wrong_group":
        bind(device, group_id=0x5678)
    if target == "unicast":
        bind(device, kind="unicast")
    if target in {"two_groups", "duplicate", "mixed", "disabled", "legacy_short"}:
        bind(device)
    if target == "two_groups":
        bind(device, group_id=0x5678)
    if target == "duplicate":
        bind(device)
    if target == "mixed":
        bind(device, kind="unicast")
    reports = collect_reports(device.p)
    click_sequence(device, button_pin, 2)
    finish_sequence(device)
    assert reports == [(1, 7)]
    assert not device.zcl_list_cmds(1, SCENES)
    assert len(device.zcl_list_cmds(1, ZCL_CLUSTER_ON_OFF)) == (2 if target == "legacy_short" else 0)


@pytest.mark.parametrize("limit", [1, 3])
@pytest.mark.parametrize("scenario", ["scene_zero", "custom_scene", "no_scene_binding", "master_disabled", "not_toggle"])
def test_single_click_scene_is_optional_and_follows_toggle(device, button_pin, limit, scenario):
    set_scenes(device, enabled=int(scenario != "master_disabled"))
    setup_button(device, 1, limit)
    device.write_zigbee_attr(1, SWITCH_CFG, SINGLE_ENABLED, 1)
    if scenario == "custom_scene":
        device.write_zigbee_attr(1, SWITCH_CFG, SINGLE, 255)
    if scenario == "not_toggle":
        device.zcl_switch_actions_set(1, ZCL_ONOFF_CONFIGURATION_SWITCH_ACTION_ONOFF)
    if scenario != "no_scene_binding":
        bind(device)
    reports = collect_reports(device.p)

    click_sequence(device, button_pin, 1)
    if limit > 1:
        assert device.zcl_list_cmds(1) == [] and reports == []
        finish_sequence(device)

    onoff_command = ZCL_CMD_ONOFF_ON if scenario == "not_toggle" else ZCL_CMD_ONOFF_TOGGLE
    expected = [(ZCL_CLUSTER_ON_OFF, onoff_command, b"")]
    if scenario in {"scene_zero", "custom_scene"}:
        scene = 255 if scenario == "custom_scene" else 0
        expected.append((SCENES, 0x05, bytes([0x34, 0x12, scene, 0xFF, 0xFF])))
    assert [(cmd.cluster, cmd.cmd, cmd.data) for cmd in device.zcl_list_cmds(1)] == expected
    assert reports == [(1, 5)]


def test_single_click_config_preserves_v1_scenes_and_persists(device_config):
    legacy = Path("stub_nvm_data/item_28.bin")
    legacy.parent.mkdir(exist_ok=True)
    legacy_data = bytes([1, 1, 0x34, 0x12, 7, 8])
    legacy.write_bytes(legacy_data)
    with StubProc(device_config=device_config) as proc:
        device = Device(proc)
        assert device.read_zigbee_attr(1, SWITCH_CFG, ENABLED) == "1"
        assert device.read_zigbee_attr(1, SWITCH_CFG, GROUP) == "4660"
        assert device.read_zigbee_attr(1, SWITCH_CFG, DOUBLE) == "7"
        assert device.read_zigbee_attr(1, SWITCH_CFG, TRIPLE) == "8"
        assert device.read_zigbee_attr(1, SWITCH_CFG, SINGLE_ENABLED) == "0"
        assert device.read_zigbee_attr(1, SWITCH_CFG, SINGLE) == "0"
        assert legacy.read_bytes() == legacy_data
        device.write_zigbee_attr(1, SWITCH_CFG, SINGLE, 42)
        device.write_zigbee_attr(1, SWITCH_CFG, SINGLE_ENABLED, 1)

    with StubProc(device_config=device_config) as proc:
        device = Device(proc)
        assert device.read_zigbee_attr(1, SWITCH_CFG, SINGLE) == "42"
        assert device.read_zigbee_attr(1, SWITCH_CFG, SINGLE_ENABLED) == "1"
        assert device.read_zigbee_attr(1, SWITCH_CFG, DOUBLE) == "7"
        assert device.read_zigbee_attr(1, SWITCH_CFG, TRIPLE) == "8"


@pytest.mark.parametrize("writes", [[(DOUBLE, 9)], [(ENABLED, 0), (ENABLED, 1)], [(DOUBLE, 1)]])
def test_scene_config_changes_disarm_pending_recall(device, button_pin, writes):
    set_scenes(device)
    bind(device)
    reports = collect_reports(device.p)
    click_sequence(device, button_pin, 2)
    for attr, value in writes:
        device.write_zigbee_attr(1, SWITCH_CFG, attr, value)
    finish_sequence(device)
    assert reports == [(1, 7)]
    commands = device.zcl_list_cmds(1, SCENES)
    assert [cmd.data for cmd in commands] == ([bytes.fromhex("341201ffff")] if writes == [(DOUBLE, 1)] else [])


def test_scene_and_level_control_are_independent(device, button_pin):
    set_scenes(device)
    bind(device)
    reports = collect_reports(device.p)
    click_sequence(device, button_pin, 1)
    device.press_button(button_pin)
    device.step_time(900)
    device.release_button(button_pin)
    finish_sequence(device)
    assert reports == [(1, 8), (1, 9)]
    assert [cmd.cmd for cmd in device.zcl_list_cmds(1, ZCL_CLUSTER_LEVEL_CONTROL)] == [
        ZCL_CMD_LEVEL_MOVE_WITH_ON_OFF, ZCL_CMD_LEVEL_STOP_WITH_ON_OFF,
    ]
    assert not device.zcl_list_cmds(1, SCENES)
    assert not device.zcl_list_cmds(1, ZCL_CLUSTER_ON_OFF)


@pytest.mark.parametrize("failure", ["offline", "timer"])
def test_scene_does_not_leak_after_send_or_timer_failure(device, button_pin, failure):
    set_scenes(device)
    bind(device)
    if failure == "offline":
        assert device.p.exec("net 0").ok
        click_sequence(device, button_pin, 2)
    else:
        click_sequence(device, button_pin, 1)
        assert device.p.exec("task_reject 1").ok
        click_sequence(device, button_pin, 1)
    finish_sequence(device)
    assert not device.zcl_list_cmds(1, SCENES)
    assert device.p.exec("net 1" if failure == "offline" else "task_reject 0").ok
    click_sequence(device, button_pin, 1)
    finish_sequence(device)
    assert not device.zcl_list_cmds(1, SCENES)


def test_scene_config_persistence_validation_and_failed_save(device_config):
    with StubProc(device_config=device_config) as proc:
        device = Device(proc)
        assert device.read_zigbee_attr(1, SWITCH_CFG, ENABLED) == "0"
        assert device.read_zigbee_attr(1, SWITCH_CFG, GROUP) == "0"
        assert device.read_zigbee_attr(1, SWITCH_CFG, DOUBLE) == "1"
        assert device.read_zigbee_attr(1, SWITCH_CFG, TRIPLE) == "2"
        device.write_zigbee_attr(1, SWITCH_CFG, ENABLED, 1)
        assert device.read_zigbee_attr(1, SWITCH_CFG, ENABLED) == "0"
        device.write_zigbee_attr(1, SWITCH_CFG, GROUP, 0xFFF8)
        assert device.read_zigbee_attr(1, SWITCH_CFG, GROUP) == "0"
        failed = Path("stub_nvm_data/item_28.bin")
        failed.mkdir(parents=True, exist_ok=True)
        device.write_zigbee_attr(1, SWITCH_CFG, GROUP, 0x1234)
        assert device.read_zigbee_attr(1, SWITCH_CFG, GROUP) == "0"
        failed.rmdir()
        set_scenes(device)
        device.write_zigbee_attr(1, SWITCH_CFG, GROUP, 0)
        assert device.read_zigbee_attr(1, SWITCH_CFG, GROUP) == "4660"
        device.write_zigbee_attr(1, SWITCH_CFG, DOUBLE, 0)
        device.write_zigbee_attr(1, SWITCH_CFG, TRIPLE, 255)

    with StubProc(device_config=device_config) as proc:
        device = Device(proc)
        bind(device)
        assert device.read_zigbee_attr(1, SWITCH_CFG, ENABLED) == "1"
        assert device.read_zigbee_attr(1, SWITCH_CFG, GROUP) == "4660"
        assert device.read_zigbee_attr(1, SWITCH_CFG, DOUBLE) == "0"
        assert device.read_zigbee_attr(1, SWITCH_CFG, TRIPLE) == "255"
        assert device.read_zigbee_attr(2, SWITCH_CFG, ENABLED) == "0"
        click_sequence(device, "A0", 2)
        finish_sequence(device)
        device.step_time(2000)
        click_sequence(device, "A0", 3)
        finish_sequence(device)
        assert [c.data for c in device.zcl_list_cmds(1, SCENES)] == [bytes.fromhex("341200ffff"), bytes.fromhex("3412ffffff")]


FOUR_GANG = "StubManufacturer;StubScenes;LC0;SA0u;SA1u;SA2u;SA3u;RB0;RB1;RB2;RB3;ID0;ID1;ID2;ID3;"


@pytest.mark.parametrize("stub", ["stub_device", "stub_end_device"])
def test_four_gang_scenes_keep_ota_and_existing_endpoints(stub):
    with StubProc(cmd=[f"./build/stub/{stub}"], device_config=FOUR_GANG) as proc:
        device = Device(proc)
        expect_descriptor(proc, 1, 0, cluster=0x0019)  # OTA upgrade client
        if stub == "stub_end_device":
            expect_descriptor(proc, 1, 1, cluster=0x0020)  # Poll Control server
        for ep in range(1, 5):
            expect_descriptor(proc, ep, 0)
            expect_descriptor(proc, ep, 0, cluster=ZCL_CLUSTER_ON_OFF)
            expect_descriptor(proc, ep, 0, cluster=ZCL_CLUSTER_LEVEL_CONTROL)
            assert device.read_zigbee_attr(ep, SWITCH_CFG, ENABLED) == "0"
            setup_button(device, ep, 2)
            device.zcl_switch_relay_mode_set(ep, ZCL_ONOFF_CONFIGURATION_RELAY_MODE_DETACHED)
            device.zcl_switch_binding_mode_set(ep, ZCL_ONOFF_CONFIGURATION_BINDED_MODE_SINGLE_CLICK)
            device.write_zigbee_attr(ep, SWITCH_CFG, GROUP, 0x1200 + ep)
            device.write_zigbee_attr(ep, SWITCH_CFG, DOUBLE, 10 + ep)
            device.write_zigbee_attr(ep, SWITCH_CFG, ENABLED, 1)
            bind(device, group_id=0x1200 + ep, ep=ep)

        for ep in range(5, 9):
            expect_descriptor(proc, ep, None)
            expect_descriptor(proc, ep, 1, cluster=ZCL_CLUSTER_ON_OFF)
            expect_descriptor(proc, ep, 1, cluster=0x0004)  # Groups server
            assert device.zcl_relay_get(ep) == "0"

        for ep in range(1, 5):
            click_sequence(device, f"A{ep - 1}", 2)
            finish_sequence(device)

        assert [(cmd.ep, cmd.data) for cmd in device.zcl_list_cmds(cluster=SCENES)] == [
            (ep, bytes([ep, 0x12, 10 + ep, 0xFF, 0xFF])) for ep in range(1, 5)
        ]
        assert not device.zcl_list_cmds(cluster=ZCL_CLUSTER_ON_OFF)


def test_scenes_do_not_displace_existing_descriptors_when_attribute_table_is_full():
    # Four indicator-equipped relays already have 127 attributes with Scenes;
    # the three extra battery attributes push this synthetic layout past 128.
    config = FOUR_GANG + "BTC5;"
    with StubProc(cmd=["./build/stub/stub_end_device"], device_config=config) as proc:
        device = Device(proc)
        expect_descriptor(proc, 1, 0, cluster=0x0019)
        for ep in range(1, 5):
            expect_descriptor(proc, ep, None)
            assert device.zcl_switch_get_multistate_value(ep) == "0"
            assert not proc.exec(f"zcl_read {ep} {SWITCH_CFG:x} {ENABLED:x}").ok
        for ep in range(5, 9):
            assert device.zcl_relay_get(ep) == "0"
