// Device bindings and optional generation credentials are independent services.
// A failed credential migration must not lock the entire desktop application.
export async function loadDesktopStartup(initializeConfigs, loadBindings) {
  const [configs, bindings] = await Promise.allSettled([
    Promise.resolve().then(initializeConfigs),
    Promise.resolve().then(loadBindings),
  ]);
  if (bindings.status === "rejected") throw bindings.reason;
  return {
    bindings: bindings.value,
    configError: configs.status === "rejected"
      ? configs.reason?.message || String(configs.reason)
      : "",
  };
}
