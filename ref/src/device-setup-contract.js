/**
 * [Input] Consume device setup fixtures defined by `ref/src/fixtures.js`[Pos].
 * [Output] Provide the stage-one setup state contract, mock state machine helpers, and normalized snapshot adapter for the Pet Manager bind flow.
 * [Pos] contract node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import { DEVICE_SETUP_FIXTURES } from "./fixtures.js";
import { normalizeText } from "./lib/normalize-text.js";

export const SETUP_STATES = Object.freeze({
  setupUninitialized: "setup_uninitialized",
  setupWaitHostLink: "setup_wait_host_link",
  setupHostLinkAttached: "setup_host_link_attached",
  setupDeviceIdentifying: "setup_device_identifying",
  setupDeviceIdentified: "setup_device_identified",
  setupWaitNetworkInput: "setup_wait_network_input",
  setupNetworkProvisioning: "setup_network_provisioning",
  setupNetworkVerifying: "setup_network_verifying",
  setupDeviceRegistering: "setup_device_registering",
  setupBindingPersisting: "setup_binding_persisting",
  setupChannelVerifying: "setup_channel_verifying",
  setupCompleted: "setup_completed",
  setupErrorHostLink: "setup_error_host_link",
  setupErrorDeviceIdentify: "setup_error_device_identify",
  setupErrorNetwork: "setup_error_network",
  setupErrorRegistration: "setup_error_registration",
  setupErrorChannel: "setup_error_channel",
});

export const HOST_LINK_STATES = Object.freeze({
  detached: "host_link_detached",
  attached: "host_link_attached",
  unstable: "host_link_unstable",
});

export const NETWORK_STATES = Object.freeze({
  unknown: "network_unknown",
  waitCredentials: "network_wait_credentials",
  connecting: "network_connecting",
  online: "network_online",
  failed: "network_failed",
});

export const BINDING_STATES = Object.freeze({
  unbound: "binding_unbound",
  registering: "binding_registering",
  bound: "binding_bound",
  failed: "binding_failed",
});

export const STREAM_STATES = Object.freeze({
  unverified: "stream_unverified",
  verifying: "stream_verifying",
  active: "stream_active",
  stale: "stream_stale",
  failed: "stream_failed",
});

export const DEVICE_SETUP_SCENARIOS = Object.freeze({
  happyPath: "happy_path",
  hostLinkError: "host_link_error",
  networkError: "network_error",
  channelError: "channel_error",
});

export const DEVICE_SETUP_SCENARIO_OPTIONS = Object.freeze([
  {
    id: DEVICE_SETUP_SCENARIOS.happyPath,
    label: "标准路径",
    description: "完整走通连接、配网、登记和通道验证。",
  },
  {
    id: DEVICE_SETUP_SCENARIOS.hostLinkError,
    label: "连接异常",
    description: "在主机链路阶段故障，验证首段恢复体验。",
  },
  {
    id: DEVICE_SETUP_SCENARIOS.networkError,
    label: "配网异常",
    description: "在联网验证阶段故障，保留 Wi-Fi 输入重试。",
  },
  {
    id: DEVICE_SETUP_SCENARIOS.channelError,
    label: "通道异常",
    description: "在消息通道验证阶段故障，验证最终恢复流程。",
  },
]);

export const DEVICE_SETUP_STEPS = Object.freeze([
  { id: "host-link", title: "连接设备" },
  { id: "identify", title: "识别设备" },
  { id: "network", title: "网络配置" },
  { id: "binding", title: "绑定验证" },
  { id: "completed", title: "完成" },
]);

const SETUP_STATE_VALUES = new Set(Object.values(SETUP_STATES));
const HOST_LINK_STATE_VALUES = new Set(Object.values(HOST_LINK_STATES));
const NETWORK_STATE_VALUES = new Set(Object.values(NETWORK_STATES));
const BINDING_STATE_VALUES = new Set(Object.values(BINDING_STATES));
const STREAM_STATE_VALUES = new Set(Object.values(STREAM_STATES));

const STATE_TO_STEP_INDEX = {
  [SETUP_STATES.setupUninitialized]: 0,
  [SETUP_STATES.setupWaitHostLink]: 0,
  [SETUP_STATES.setupHostLinkAttached]: 0,
  [SETUP_STATES.setupDeviceIdentifying]: 1,
  [SETUP_STATES.setupDeviceIdentified]: 1,
  [SETUP_STATES.setupErrorDeviceIdentify]: 1,
  [SETUP_STATES.setupWaitNetworkInput]: 2,
  [SETUP_STATES.setupNetworkProvisioning]: 2,
  [SETUP_STATES.setupNetworkVerifying]: 2,
  [SETUP_STATES.setupErrorNetwork]: 2,
  [SETUP_STATES.setupDeviceRegistering]: 3,
  [SETUP_STATES.setupBindingPersisting]: 3,
  [SETUP_STATES.setupChannelVerifying]: 3,
  [SETUP_STATES.setupErrorRegistration]: 3,
  [SETUP_STATES.setupErrorChannel]: 3,
  [SETUP_STATES.setupCompleted]: 4,
};

const PROCESSING_DELAYS = {
  [SETUP_STATES.setupHostLinkAttached]: 900,
  [SETUP_STATES.setupDeviceIdentifying]: 1100,
  [SETUP_STATES.setupNetworkProvisioning]: 950,
  [SETUP_STATES.setupNetworkVerifying]: 1050,
  [SETUP_STATES.setupDeviceRegistering]: 900,
  [SETUP_STATES.setupBindingPersisting]: 900,
  [SETUP_STATES.setupChannelVerifying]: 1000,
};

const SETUP_STATE_META = {
  [SETUP_STATES.setupUninitialized]: {
    tone: "neutral",
    eyebrow: "连接副屏设备",
    title: "准备开始设备接入",
    description: "状态机会从连接、识别、网络配置、绑定验证一路推进到完成态。",
    primaryActionLabel: "开始连接",
  },
  [SETUP_STATES.setupWaitHostLink]: {
    tone: "neutral",
    eyebrow: "步骤 1 / 连接设备",
    title: "等待电脑建立主机链路",
    description: "把副屏设备通过 USB Type-C 或约定连接方式接到当前电脑后，再启动接入流程。",
    primaryActionLabel: "开始连接",
  },
  [SETUP_STATES.setupHostLinkAttached]: {
    tone: "info",
    eyebrow: "步骤 1 / 连接设备",
    title: "已检测到主机链路",
    description: "Pet Manager 正在确认主机链路的稳定性，并准备读取 board 信息。",
  },
  [SETUP_STATES.setupDeviceIdentifying]: {
    tone: "info",
    eyebrow: "步骤 2 / 识别设备",
    title: "正在读取设备标识",
    description: "继续读取 board_device_id、设备标签和当前接入信息。",
  },
  [SETUP_STATES.setupDeviceIdentified]: {
    tone: "info",
    eyebrow: "步骤 2 / 识别设备",
    title: "设备识别完成",
    description: "已拿到副屏设备标识，下一步输入当前办公网络信息完成配网。",
    primaryActionLabel: "继续配网",
  },
  [SETUP_STATES.setupWaitNetworkInput]: {
    tone: "warning",
    eyebrow: "步骤 3 / 网络配置",
    title: "输入副屏设备的 Wi-Fi 信息",
    description: "本阶段只做前端原型，密码不会写入快照；SSID 会保留到最终完成态。",
    primaryActionLabel: "下发 Wi‑Fi 配置",
  },
  [SETUP_STATES.setupNetworkProvisioning]: {
    tone: "info",
    eyebrow: "步骤 3 / 网络配置",
    title: "正在下发网络配置",
    description: "副屏设备正在接收当前网络配置，并切换到联网模式。",
  },
  [SETUP_STATES.setupNetworkVerifying]: {
    tone: "info",
    eyebrow: "步骤 3 / 网络配置",
    title: "正在验证网络连接",
    description: "Pet Manager 正在等待设备确认联网成功和网络回执。",
  },
  [SETUP_STATES.setupDeviceRegistering]: {
    tone: "info",
    eyebrow: "步骤 4 / 绑定验证",
    title: "正在登记设备标识",
    description: "当前主机与副屏设备的标识正在写入接入快照，准备生成绑定记录。",
  },
  [SETUP_STATES.setupBindingPersisting]: {
    tone: "info",
    eyebrow: "步骤 4 / 绑定验证",
    title: "正在保存绑定关系",
    description: "设备标识已经拿到，正在生成本地绑定记录并固化主链路。",
  },
  [SETUP_STATES.setupChannelVerifying]: {
    tone: "info",
    eyebrow: "步骤 4 / 绑定验证",
    title: "正在验证消息通道",
    description: "当前流程会检查设备端与工作台之间的状态流是否已经可用。",
  },
  [SETUP_STATES.setupCompleted]: {
    tone: "success",
    eyebrow: "步骤 5 / 完成",
    title: "副屏设备接入完成",
    description: "主机链路、设备标识、网络和消息通道都已通过当前原型验证，可以进入工作引导。",
    primaryActionLabel: "进入工作引导",
  },
  [SETUP_STATES.setupErrorHostLink]: {
    tone: "error",
    eyebrow: "步骤 1 / 连接设备",
    title: "主机链路不稳定",
    description: "当前没有稳定识别到副屏设备，请检查线缆、供电或连接方式后重新连接。",
    primaryActionLabel: "重新连接",
  },
  [SETUP_STATES.setupErrorDeviceIdentify]: {
    tone: "error",
    eyebrow: "步骤 2 / 识别设备",
    title: "设备识别失败",
    description: "主机链路已建立，但当前无法读取设备信息，请重新发起识别。",
    primaryActionLabel: "重新识别",
  },
  [SETUP_STATES.setupErrorNetwork]: {
    tone: "error",
    eyebrow: "步骤 3 / 网络配置",
    title: "网络验证失败",
    description: "设备没有确认联网成功，请检查 SSID 或密码后重新下发配置。",
    primaryActionLabel: "重新配网",
  },
  [SETUP_STATES.setupErrorRegistration]: {
    tone: "error",
    eyebrow: "步骤 4 / 绑定验证",
    title: "设备登记失败",
    description: "当前无法保存设备标识，请重新登记副屏设备。",
    primaryActionLabel: "重新登记设备",
  },
  [SETUP_STATES.setupErrorChannel]: {
    tone: "error",
    eyebrow: "步骤 4 / 绑定验证",
    title: "消息通道验证失败",
    description: "设备标识和绑定记录已经存在，但当前消息通道没有通过验证，请重新校验。",
    primaryActionLabel: "重新验证通道",
  },
};

const HOST_LINK_META = {
  [HOST_LINK_STATES.detached]: { label: "等待连接", tone: "neutral" },
  [HOST_LINK_STATES.attached]: { label: "已连接", tone: "info" },
  [HOST_LINK_STATES.unstable]: { label: "链路不稳", tone: "error" },
};

const NETWORK_META = {
  [NETWORK_STATES.unknown]: { label: "未开始", tone: "neutral" },
  [NETWORK_STATES.waitCredentials]: { label: "等待输入", tone: "warning" },
  [NETWORK_STATES.connecting]: { label: "连接中", tone: "info" },
  [NETWORK_STATES.online]: { label: "已在线", tone: "success" },
  [NETWORK_STATES.failed]: { label: "连接失败", tone: "error" },
};

const BINDING_META = {
  [BINDING_STATES.unbound]: { label: "未绑定", tone: "neutral" },
  [BINDING_STATES.registering]: { label: "登记中", tone: "info" },
  [BINDING_STATES.bound]: { label: "已绑定", tone: "success" },
  [BINDING_STATES.failed]: { label: "绑定失败", tone: "error" },
};

const STREAM_META = {
  [STREAM_STATES.unverified]: { label: "未校验", tone: "neutral" },
  [STREAM_STATES.verifying]: { label: "校验中", tone: "info" },
  [STREAM_STATES.active]: { label: "已激活", tone: "success" },
  [STREAM_STATES.stale]: { label: "状态过期", tone: "warning" },
  [STREAM_STATES.failed]: { label: "验证失败", tone: "error" },
};

const SCENARIO_FAILURE_KEYS = {
  [DEVICE_SETUP_SCENARIOS.hostLinkError]: "hostLink",
  [DEVICE_SETUP_SCENARIOS.networkError]: "network",
  [DEVICE_SETUP_SCENARIOS.channelError]: "channel",
};

function slugify(value, fallback) {
  const normalized = normalizeText(value, fallback)
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "");

  return normalized || fallback;
}

function normalizeScenarioId(value) {
  const normalized = normalizeText(value, DEVICE_SETUP_SCENARIOS.happyPath);
  return Object.values(DEVICE_SETUP_SCENARIOS).includes(normalized)
    ? normalized
    : DEVICE_SETUP_SCENARIOS.happyPath;
}

function createDefaultDesktopId(fixture) {
  return `desktop-${slugify(fixture.device_label, "workspace")}-01`;
}

function createDefaultBoardId(fixture) {
  return `board-${slugify(fixture.device_label, "side-display")}-01`;
}

function normalizeMetrics(metrics = {}) {
  return {
    token_total: Number(metrics.token_total) || 0,
    tool_call_count: Number(metrics.tool_call_count) || 0,
    code_line_delta: Number(metrics.code_line_delta) || 0,
    task_completed_count: Number(metrics.task_completed_count) || 0,
  };
}

function normalizeSetupState(value) {
  const normalized = normalizeText(value, SETUP_STATES.setupWaitHostLink);
  return SETUP_STATE_VALUES.has(normalized) ? normalized : SETUP_STATES.setupWaitHostLink;
}

function normalizeHostLinkState(value) {
  const normalized = normalizeText(value, HOST_LINK_STATES.detached);
  return HOST_LINK_STATE_VALUES.has(normalized) ? normalized : HOST_LINK_STATES.detached;
}

function normalizeNetworkState(value) {
  const normalized = normalizeText(value, NETWORK_STATES.unknown);
  return NETWORK_STATE_VALUES.has(normalized) ? normalized : NETWORK_STATES.unknown;
}

function normalizeBindingState(value) {
  const normalized = normalizeText(value, BINDING_STATES.unbound);
  return BINDING_STATE_VALUES.has(normalized) ? normalized : BINDING_STATES.unbound;
}

function normalizeStreamState(value) {
  const normalized = normalizeText(value, STREAM_STATES.unverified);
  return STREAM_STATE_VALUES.has(normalized) ? normalized : STREAM_STATES.unverified;
}

function loadFixture(scenarioId) {
  return DEVICE_SETUP_FIXTURES[scenarioId] || DEVICE_SETUP_FIXTURES.happy_path || {};
}

function buildInitialSnapshot(fixture) {
  return normalizeDeviceSetupSnapshot(
    {
      desktop_device_id: normalizeText(fixture.desktop_device_id, createDefaultDesktopId(fixture)),
      board_device_id: "",
      device_label: normalizeText(fixture.device_label, "Companion Side Display"),
      setup_state: SETUP_STATES.setupWaitHostLink,
      host_link_state: HOST_LINK_STATES.detached,
      network_state: NETWORK_STATES.unknown,
      binding_state: BINDING_STATES.unbound,
      stream_state: STREAM_STATES.unverified,
      wifi_ssid: normalizeText(fixture.wifi_ssid),
      pet_behavior_state: normalizeText(fixture.pet_behavior_state, "pet_idle_default"),
      metrics: normalizeMetrics(fixture.metrics),
      updated_at: Date.now(),
    },
    fixture,
  );
}

function markConsumed(session, failureKey) {
  return {
    ...session,
    consumedFailures: {
      ...session.consumedFailures,
      [failureKey]: true,
    },
  };
}

function patchSessionSnapshot(session, snapshotPatch = {}, sessionPatch = {}) {
  return {
    ...session,
    ...sessionPatch,
    snapshot: normalizeDeviceSetupSnapshot(
      {
        ...session.snapshot,
        ...snapshotPatch,
        updated_at: Date.now(),
      },
      session.fixture,
    ),
  };
}

function shouldFail(session, failureKey) {
  return Boolean(failureKey) && !session.consumedFailures[failureKey];
}

export function normalizeDeviceSetupSnapshot(rawSnapshot = {}, fixture = {}) {
  const normalizedFixture = fixture || {};
  return {
    desktop_device_id: normalizeText(
      rawSnapshot.desktop_device_id,
      normalizeText(normalizedFixture.desktop_device_id, createDefaultDesktopId(normalizedFixture)),
    ),
    board_device_id: normalizeText(rawSnapshot.board_device_id),
    device_label: normalizeText(rawSnapshot.device_label, normalizeText(normalizedFixture.device_label, "Companion Side Display")),
    setup_state: normalizeSetupState(rawSnapshot.setup_state),
    host_link_state: normalizeHostLinkState(rawSnapshot.host_link_state),
    network_state: normalizeNetworkState(rawSnapshot.network_state),
    binding_state: normalizeBindingState(rawSnapshot.binding_state),
    stream_state: normalizeStreamState(rawSnapshot.stream_state),
    wifi_ssid: normalizeText(rawSnapshot.wifi_ssid),
    updated_at: Number(rawSnapshot.updated_at) || Date.now(),
    pet_behavior_state: normalizeText(rawSnapshot.pet_behavior_state, normalizeText(normalizedFixture.pet_behavior_state, "pet_idle_default")),
    metrics: normalizeMetrics(rawSnapshot.metrics || normalizedFixture.metrics),
  };
}

export function createDeviceSetupSession(options = {}) {
  const scenarioId = normalizeScenarioId(options.scenarioId);
  const fixture = loadFixture(scenarioId);

  if (options.snapshot) {
    return {
      scenarioId,
      fixture,
      wifiPassword: "",
      consumedFailures: {
        hostLink: true,
        network: true,
        channel: true,
      },
      snapshot: normalizeDeviceSetupSnapshot(options.snapshot, fixture),
    };
  }

  return {
    scenarioId,
    fixture,
    wifiPassword: "",
    consumedFailures: {
      hostLink: false,
      network: false,
      channel: false,
    },
    snapshot: buildInitialSnapshot(fixture),
  };
}

export function getSetupStepIndex(setupState) {
  return STATE_TO_STEP_INDEX[normalizeSetupState(setupState)] ?? 0;
}

export function getSetupStateMeta(snapshot = {}) {
  const state = normalizeSetupState(snapshot.setup_state);
  return SETUP_STATE_META[state] || SETUP_STATE_META[SETUP_STATES.setupWaitHostLink];
}

export function getSetupSubstateMeta(kind, value) {
  if (kind === "host") {
    return HOST_LINK_META[normalizeHostLinkState(value)];
  }
  if (kind === "network") {
    return NETWORK_META[normalizeNetworkState(value)];
  }
  if (kind === "binding") {
    return BINDING_META[normalizeBindingState(value)];
  }
  if (kind === "stream") {
    return STREAM_META[normalizeStreamState(value)];
  }

  return { label: normalizeText(value, "未知"), tone: "neutral" };
}

export function isProcessingSetupState(setupState) {
  return Boolean(PROCESSING_DELAYS[normalizeSetupState(setupState)]);
}

export function getSetupProcessingDelay(setupState) {
  return PROCESSING_DELAYS[normalizeSetupState(setupState)] ?? 900;
}

export function reduceDeviceSetupSession(session, action) {
  if (!session) {
    return createDeviceSetupSession();
  }

  switch (action.type) {
    case "hydrate_snapshot":
      return createDeviceSetupSession({
        scenarioId: action.scenarioId || session.scenarioId,
        snapshot: action.snapshot,
      });
    case "set_scenario":
      return createDeviceSetupSession({ scenarioId: action.scenarioId });
    case "reset":
      return createDeviceSetupSession({ scenarioId: session.scenarioId });
    case "update_wifi_ssid":
      return patchSessionSnapshot(session, { wifi_ssid: normalizeText(action.value) });
    case "update_wifi_password":
      return {
        ...session,
        wifiPassword: String(action.value || ""),
      };
    case "advance":
      return advanceDeviceSetupSession(session);
    case "auto_progress":
      return autoProgressDeviceSetupSession(session);
    default:
      return session;
  }
}

function advanceDeviceSetupSession(session) {
  const { snapshot } = session;

  switch (snapshot.setup_state) {
    case SETUP_STATES.setupUninitialized:
    case SETUP_STATES.setupWaitHostLink:
    case SETUP_STATES.setupErrorHostLink:
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupHostLinkAttached,
        host_link_state: HOST_LINK_STATES.attached,
      });
    case SETUP_STATES.setupErrorDeviceIdentify:
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupDeviceIdentifying,
        host_link_state: HOST_LINK_STATES.attached,
      });
    case SETUP_STATES.setupDeviceIdentified:
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupWaitNetworkInput,
        network_state: NETWORK_STATES.waitCredentials,
      });
    case SETUP_STATES.setupWaitNetworkInput:
      if (!snapshot.wifi_ssid || !normalizeText(session.wifiPassword)) {
        return session;
      }
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupNetworkProvisioning,
        network_state: NETWORK_STATES.connecting,
      });
    case SETUP_STATES.setupErrorNetwork:
      if (!snapshot.wifi_ssid || !normalizeText(session.wifiPassword)) {
        return session;
      }
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupNetworkProvisioning,
        network_state: NETWORK_STATES.connecting,
      });
    case SETUP_STATES.setupErrorRegistration:
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupDeviceRegistering,
        binding_state: BINDING_STATES.registering,
      });
    case SETUP_STATES.setupErrorChannel:
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupChannelVerifying,
        binding_state: BINDING_STATES.bound,
        stream_state: STREAM_STATES.verifying,
      });
    default:
      return session;
  }
}

function autoProgressDeviceSetupSession(session) {
  const { snapshot, scenarioId, fixture } = session;
  const failureKey = SCENARIO_FAILURE_KEYS[scenarioId];

  switch (snapshot.setup_state) {
    case SETUP_STATES.setupHostLinkAttached:
      if (shouldFail(session, failureKey === "hostLink" ? failureKey : "")) {
        return markConsumed(
          patchSessionSnapshot(session, {
            setup_state: SETUP_STATES.setupErrorHostLink,
            host_link_state: HOST_LINK_STATES.unstable,
          }),
          "hostLink",
        );
      }
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupDeviceIdentifying,
        host_link_state: HOST_LINK_STATES.attached,
      });
    case SETUP_STATES.setupDeviceIdentifying:
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupDeviceIdentified,
        board_device_id: normalizeText(snapshot.board_device_id, normalizeText(fixture.board_device_id, createDefaultBoardId(fixture))),
        device_label: normalizeText(fixture.device_label, snapshot.device_label),
      });
    case SETUP_STATES.setupNetworkProvisioning:
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupNetworkVerifying,
        network_state: NETWORK_STATES.connecting,
      });
    case SETUP_STATES.setupNetworkVerifying:
      if (shouldFail(session, failureKey === "network" ? failureKey : "")) {
        return markConsumed(
          patchSessionSnapshot(session, {
            setup_state: SETUP_STATES.setupErrorNetwork,
            network_state: NETWORK_STATES.failed,
          }),
          "network",
        );
      }
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupDeviceRegistering,
        network_state: NETWORK_STATES.online,
        binding_state: BINDING_STATES.registering,
      });
    case SETUP_STATES.setupDeviceRegistering:
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupBindingPersisting,
        binding_state: BINDING_STATES.registering,
      });
    case SETUP_STATES.setupBindingPersisting:
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupChannelVerifying,
        binding_state: BINDING_STATES.bound,
        stream_state: STREAM_STATES.verifying,
      });
    case SETUP_STATES.setupChannelVerifying:
      if (shouldFail(session, failureKey === "channel" ? failureKey : "")) {
        return markConsumed(
          patchSessionSnapshot(session, {
            setup_state: SETUP_STATES.setupErrorChannel,
            binding_state: BINDING_STATES.bound,
            stream_state: STREAM_STATES.failed,
          }),
          "channel",
        );
      }
      return patchSessionSnapshot(session, {
        setup_state: SETUP_STATES.setupCompleted,
        binding_state: BINDING_STATES.bound,
        stream_state: STREAM_STATES.active,
      });
    default:
      return session;
  }
}
