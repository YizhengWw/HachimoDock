(function attachPetClawDetailView(global) {
    function renderPetClawDetailViewSection(root) {
        if (!root) return;

        root.innerHTML = `
            <div class="container">
                <div class="detail-screen">
                    <aside class="detail-status-panel" aria-label="状态页">
                        <div class="detail-status-copy-block">
                            <div class="pet-status-kicker">
                                <span class="status-dot"></span>
                                PET STATUS
                            </div>
                            <div class="pet-status-name" id="detailAgentName">梗犬小助手</div>
                            <div class="pet-status-copy" id="detailStatusCopy">当前接入 2 个 CLI 渠道 · Codex、Gemini · 累计 22,339 TOKEN · 工具调用 1 次 · 更新 12:53:33 · 刚刚</div>
                        </div>
                        <div class="pet-status-grid">
                            <article class="pet-status-metric">
                                <div class="metric-header">
                                    <span class="metric-label">成长等级</span>
                                    <span class="level-badge" id="detailPetLevel">LV 1</span>
                                </div>
                                <strong id="detailSatietyValue">梗犬管家</strong>
                                <div class="pet-status-inline-note">经验值 <span id="detailExpProgress">0%</span></div>
                            </article>
                            <article class="pet-status-metric">
                                <div class="metric-header">
                                    <span class="metric-label">交互反馈</span>
                                    <i class="icon-pulse"></i>
                                </div>
                                <strong id="detailCodeLinesValue">保持专注</strong>
                                <div class="pet-status-inline-note">工具已调用 <span id="detailToolCalls">0</span> 次</div>
                            </article>
                            <article class="pet-status-metric span-2">
                                <div class="metric-header">
                                    <span class="metric-label">核心代谢与资源消耗</span>
                                    <i class="icon-billing"></i>
                                </div>
                                <div class="vitality-row">
                                    <div class="vital-item">
                                        <span class="vital-label">脑力负荷</span>
                                        <strong id="detailBrainLoad">0%</strong>
                                    </div>
                                    <div class="vital-item">
                                        <span class="vital-label">记忆留存</span>
                                        <strong id="detailMemoryHit">0%</strong>
                                    </div>
                                    <div class="vital-item">
                                        <span class="vital-label">总能量值</span>
                                        <strong id="detailSpendValue">0 <small>TOKEN</small></strong>
                                    </div>
                                </div>
                                <div class="pet-status-app-list" id="detailAppSpendList">
                                    <!-- 动态生成的渠道明细 -->
                                </div>
                            </article>
                        </div>
                    </aside>
                </div>
            </div>
        `;
    }

    global.renderPetClawDetailViewSection = renderPetClawDetailViewSection;
})(window);
