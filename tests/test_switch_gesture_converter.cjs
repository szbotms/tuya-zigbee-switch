// Run with the zigbee-herdsman-converters and zigbee-herdsman versions used by Z2M.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const {prepareDefinition} = require("zigbee-herdsman-converters");
const {Zcl} = require("zigbee-herdsman");
const {attributeKeyValue} = require(path.join(path.dirname(require.resolve("zigbee-herdsman")),
    "controller/helpers/zclFrameConverter.js"));

const converter = {exports: {}};
const source = fs.readFileSync(path.join(__dirname, "../zigbee2mqtt/converters/switch_custom.js"), "utf8");
new Function("require", "module", source)(require, converter);

const definitions = converter.exports.map(prepareDefinition);
const model = definitions.find((d) => d.zigbeeModel?.includes("TS0011-avatto"));
const gesture = model.fromZigbee.find((c) => c.cluster === "genMultistateInput" && c.type.length === 1);
assert.deepEqual(gesture.type, ["attributeReport"]);
const device = {customClusters: {}};
const exposes = typeof model.exposes === "function" ? model.exposes(device, {}) : model.exposes;
const meta = {device, mapped: model, endpoint_name: "switch"};
const expected = {
    2: "single_hold", 3: "position_on", 4: "position_off",
    5: "single", 6: "single_release", 7: "double", 8: "double_hold", 9: "double_release",
    10: "triple", 11: "triple_hold", 12: "triple_release",
};
const eventData = (value) => {
    // Global Report Attributes, server to client, UINT16. Verify herdsman
    // leaves the unregistered manufacturer-specific attribute as a numeric key.
    const bytes = Buffer.from([0x18, 0x01, 0x0a, 0x00, 0xff, 0x21, value, 0]);
    const frame = Zcl.Frame.fromBuffer(0x0012, Zcl.Header.fromBuffer(bytes), bytes, {});
    return attributeKeyValue(frame, 0x1141, {});
};
const publishes = (endpoint, value) => {
    const msg = {endpoint: {ID: endpoint}, data: value === undefined ? {} : eventData(value), type: "attributeReport"};
    return gesture.convert(model, msg);
};
for (const [value, suffix] of Object.entries(expected)) {
    assert.deepEqual(publishes(1, Number(value)), {action: `switch_${suffix}`});
    assert.equal(publishes(2, Number(value)), undefined); // Relay endpoint.
}
for (const value of [undefined, 0, 1, 99]) assert.equal(publishes(1, value), undefined);
for (const value of [0, 1, 2, 3, 4, 8, 11]) {
    const msg = {endpoint: {ID: 1}, data: {presentValue: value}, type: "attributeReport"};
    assert.equal(gesture.convert(model, msg), undefined);
    assert.equal(gesture.convert(model, msg), undefined); // Periodic repeat.
}
const actionExpose = exposes.find((e) => e.property === "action");
assert.deepEqual(new Set(actionExpose.values), new Set(Object.values(expected).map((s) => `switch_${s}`)));
assert(exposes.some((e) => e.property === "switch_press_action_switch"));

const multi = definitions.find((d) => d.endpoint?.(device).switch_right === 2);
const multiGesture = multi.fromZigbee.find((c) => c.cluster === "genMultistateInput" && c.type.length === 1);
assert.deepEqual(multiGesture.convert(multi, {endpoint: {ID: 2}, data: eventData(7)}), {action: "switch_right_double"});

async function verifyConfig() {
    for (const [name, id, value, type] of [
        ["max_press_count", 0xff07, 2, 0x20],
        ["confirm_release_ms", 0xff08, 200, 0x21],
    ]) {
        assert(exposes.some((e) => e.property === `${name}_switch`));
        const tz = model.toZigbee.find((c) => c.key.includes(name));
        const endpoint = {
            ID: 1,
            write: async (cluster, payload) => assert.deepEqual([cluster, payload], ["genOnOffSwitchCfg", {[id]: {value, type}}]),
            read: async (cluster, attrs) => assert.deepEqual([cluster, attrs], ["genOnOffSwitchCfg", [id]]),
        };
        await tz.convertSet(endpoint, name, value, meta);
        await tz.convertGet(endpoint, name, meta);
        const msg = {endpoint, data: {[id]: value}, type: "readResponse"};
        const outputs = model.fromZigbee.filter((c) => c.cluster === "genOnOffSwitchCfg" && c.type.includes(msg.type))
            .map((c) => c.convert(model, msg, undefined, {}, meta));
        assert.deepEqual(outputs.filter(Boolean), [{[`${name}_switch`]: value}]);
    }

    const key = "switch_binded_mode";
    const property = `${key}_switch`;
    const bindingModes = {press_start: 1, long_press: 2, short_press: 3, single_click: 4};
    assert.deepEqual(new Set(exposes.find((e) => e.property === property).values), new Set(Object.keys(bindingModes)));
    const tz = model.toZigbee.find((c) => c.key.includes(key));
    for (const [name, value] of Object.entries(bindingModes)) {
        let probes = 0;
        const endpoint = {
            ID: 1,
            write: async (cluster, payload) => assert.deepEqual([cluster, payload], ["genOnOffSwitchCfg", {0xff05: {value, type: 0x30}}]),
            read: async (cluster, attrs) => {
                assert.equal(cluster, "genOnOffSwitchCfg");
                if (attrs[0] === 0xff09) {
                    probes++;
                    return {0xff09: true};
                }
                assert.deepEqual(attrs, [0xff05]);
            },
        };
        device.getEndpoint = (id) => {
            assert.equal(id, 1);
            return endpoint;
        };
        await tz.convertSet(endpoint, key, name, meta);
        assert.equal(probes, name === "single_click" ? 1 : 0);
        await tz.convertGet(endpoint, key, meta);
        const msg = {endpoint, data: {0xff05: value}, type: "readResponse"};
        const outputs = model.fromZigbee.filter((c) => c.cluster === "genOnOffSwitchCfg" && c.type.includes(msg.type))
            .map((c) => c.convert(model, msg, undefined, {}, meta));
        assert.deepEqual(outputs.filter(Boolean), [{[property]: name}]);
    }

    for (const read of [async () => { throw new Error("Unsupported attribute"); }, async () => ({}), async () => ({0xff09: false})]) {
        const endpoint = {read, write: async () => assert.fail("unsupported firmware was written")};
        device.getEndpoint = () => endpoint;
        await assert.rejects(tz.convertSet(endpoint, key, "single_click", meta), /Cannot verify single_click support/);
    }
    // Even a previously successful capability probe must not survive a downgrade.
    let installed = true;
    const endpoint = {
        read: async () => installed ? {0xff09: 1} : {},
        write: async () => assert.equal(installed, true),
    };
    device.getEndpoint = () => endpoint;
    await tz.convertSet(endpoint, key, "single_click", meta);
    installed = false;
    await assert.rejects(tz.convertSet(endpoint, key, "single_click", meta), /Cannot verify single_click support/);
}

async function verifyScenes() {
    const scenes = model.toZigbee.find((c) => c.key.includes("scene_enabled"));
    const prefix = "genOnOffSwitchCfg";
    const attrs = {0xff0a: 0, 0xff0b: 0, 0xff0c: 1, 0xff0d: 2, 0xff0e: 0, 0xff0f: 0};
    const writes = [];
    let sceneClient = true;
    let rejectWrite = false;
    const ep = {
        ID: 1,
        supportsOutputCluster: (cluster) => cluster === "genScenes" && sceneClient,
        read: async (cluster, ids) => {
            assert.equal(cluster, prefix);
            return Object.fromEntries(ids.filter((id) => attrs[id] !== undefined).map((id) => [id, attrs[id]]));
        },
        write: async (cluster, payload) => {
            assert.equal(cluster, prefix);
            writes.push(payload);
            if (!rejectWrite) for (const [id, entry] of Object.entries(payload)) attrs[id] = entry.value;
        },
    };
    const device = {customClusters: {}, getEndpoint: (id) => id === 1 ? ep : undefined};
    const meta = {device, mapped: model, endpoint_name: "switch"};
    const properties = () => model.exposes(device, {}).map((e) => e.property);
    for (const [key, value, id, type] of [
        ["scene_group_id", 0x1234, 0xff0b, 0x21],
        ["double_click_scene_id", 0, 0xff0c, 0x20],
        ["triple_click_scene_id", 255, 0xff0d, 0x20],
        ["scene_enabled", true, 0xff0a, 0x10],
        ["single_click_scene_id", 255, 0xff0f, 0x20],
        ["single_click_scene_enabled", true, 0xff0e, 0x10],
    ]) {
        assert(properties().includes(`${key}_switch`));
        assert.deepEqual(await scenes.convertSet(ep, key, value, meta), {state: {[key]: value}});
        assert.deepEqual(writes.at(-1), {[id]: {type, value: Number(value)}});
        await scenes.convertGet(ep, key, meta);
        const msg = {endpoint: ep, data: {[id]: attrs[id]}, type: "readResponse"};
        const found = model.fromZigbee.filter((c) => c.cluster === prefix && c.type.includes(msg.type))
            .map((c) => c.convert(model, msg, undefined, {}, meta)).filter(Boolean);
        assert.deepEqual(found, [{[`${key}_switch`]: value}]);
    }
    await assert.rejects(scenes.convertSet(ep, "scene_group_id", 0xfff8, meta), /Invalid scene_group_id/);
    await assert.rejects(scenes.convertSet(ep, "double_click_scene_id", 256, meta), /Invalid double_click_scene_id/);
    await assert.rejects(scenes.convertSet(ep, "scene_enabled", 1, meta), /Invalid scene_enabled/);
    await assert.rejects(scenes.convertSet(ep, "single_click_scene_id", 256, meta), /Invalid single_click_scene_id/);
    const original = writes.length;
    sceneClient = false;
    assert(!properties().includes("scene_enabled_switch"));
    await assert.rejects(scenes.convertSet(ep, "scene_enabled", false, meta), /supports Scenes/);
    sceneClient = true;
    delete attrs[0xff0c];
    delete attrs[0xff0e];
    await assert.rejects(scenes.convertSet(ep, "double_click_scene_id", 1, meta), /did not confirm support/);
    await assert.rejects(scenes.convertSet(ep, "single_click_scene_enabled", true, meta), /did not confirm support/);
    assert.equal(writes.length, original);
    rejectWrite = true;
    await assert.rejects(scenes.convertSet(ep, "scene_group_id", 0, meta), /was not applied/);

    // Same Recall payload as the End Device stub, decoded with real herdsman.
    const bytes = Buffer.from("115a05341201ffff", "hex");
    const frame = Zcl.Frame.fromBuffer(5, Zcl.Header.fromBuffer(bytes), bytes, {});
    assert.equal(frame.command.name, "recall");
    assert.equal(frame.payload.groupid, 0x1234);
    assert.equal(frame.payload.sceneid, 1);
    const serialized = Zcl.Frame.create(Zcl.FrameType.SPECIFIC, Zcl.Direction.CLIENT_TO_SERVER,
        true, undefined, 0x5a, "recall", 5, {groupid: 0x1234, sceneid: 1, transitionTime: 0xffff}, {});
    assert.deepEqual(serialized.toBuffer(), bytes);
}

verifyConfig().then(verifyScenes).then(() => console.log("Switch gesture and Scenes converter checks passed"), (error) => {
    console.error(error);
    process.exitCode = 1;
});
