import React from "react";

// Wrap the entire page, not just its modal: modal prop expressions are evaluated
// by ComponentCenter before a child boundary could catch their errors.
export default class ComponentCenterBoundary extends React.Component {
  state = { failed: false };

  static getDerivedStateFromError() {
    return { failed: true };
  }

  componentDidCatch(error) {
    // Do not persist component data, local paths or user content in diagnostics.
    console.error("[ComponentCenter] render failed", error instanceof ReferenceError ? "ReferenceError" : "RenderError");
  }

  render() {
    if (!this.state.failed) return this.props.children;
    return <section className="component-center-recovery" role="alert">
      <h2>组件页面暂时无法显示</h2>
      <p>界面显示遇到异常。可以重新打开组件中心，或通过上方导航切换页面；不会删除组件或设备数据。</p>
      <button type="button" className="btn btn-primary" onClick={() => this.setState({ failed: false })}>重新打开组件中心</button>
    </section>;
  }
}
