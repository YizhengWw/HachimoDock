/**
 * [Input] Return callback for the app-level community import help surface.
 * [Output] In-app standalone explanation page for community/Codex pet imports.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React from "react";
import { ArrowLeft, ExternalLink } from "lucide-react";

const HELP_IMAGES = {
  codexPlay01: "/community-help/codex-play-01.png",
  codexPlay02: "/community-help/codex-play-02.png",
  codexPlay03: "/community-help/codex-play-03.png",
  codexPlay04: "/community-help/codex-play-04.png",
  importEntry: "/community-help/import-entry.png",
  petsFolder: "/community-help/pets-folder-in-settings.png",
};

function Figure({ src, alt, caption }) {
  return (
    <figure className="help-figure">
      <img src={src} alt={alt} />
      <figcaption>{caption}</figcaption>
    </figure>
  );
}

export default function CommunityImportHelp({ onBack }) {
  return (
    <div className="page page-help">
      <div className="page-toolbar">
        <button className="btn-ghost" onClick={onBack}>
          <ArrowLeft size={16} />
          返回
        </button>
      </div>

      <header className="help-hero">
        <span className="help-eyebrow">说明</span>
        <h1 className="page-hero-title">petAgent 兼容 codex pet 说明</h1>
        <p className="page-hero-sub">
          这份说明已经内置在 HachimoDock 里，用来帮助你理解社区形象、Codex 宠物资源和管理端导入流程之间的关系。
        </p>
        <a
          className="help-link"
          href="https://petdex.crafter.run/zh"
          target="_blank"
          rel="noreferrer"
        >
          打开 Petdex 中文社区
          <ExternalLink size={14} />
        </a>
      </header>

      <section className="help-section">
        <h2>1. 背景概览</h2>
        <p>
          Codex Pet 通常会把桌宠资源写入本机 <code>.codex/pets</code> 目录。petAgent 和 Codex
          不是同一个产品，但在授权访问本地目录后，HachimoDock 可以扫描这些已有资源，并生成管理端里的形象卡片。
        </p>
        <div className="help-note">
          petAgent 只负责读取和展示你本机已经存在的 codex pet 资源，不负责替第三方社区生成、维护或打包完整形象。
        </div>
      </section>

      <section className="help-section">
        <h2>2. Codex Pet 在哪里</h2>
        <p>
          你可以在支持宠物/外观设置的 Codex 客户端里启用桌宠，也可以从 Petdex 社区获取更多现成资源。
        </p>
        <Figure
          src={HELP_IMAGES.codexPlay01}
          alt="Codex 外观设置与显示入口"
          caption="Codex 外观设置与显示入口"
        />
      </section>

      <section className="help-section">
        <h2>3. 如何获取更多形象</h2>
        <p>
          你可以从社区安装，也可以在支持 hatch-pet 的环境中生成自己的宠物，再让 HachimoDock 去识别和导入。
        </p>
        <Figure
          src={HELP_IMAGES.codexPlay02}
          alt="安装 Hatch Pet"
          caption="先安装 Hatch Pet，再生成自己的宠物"
        />
        <Figure
          src={HELP_IMAGES.codexPlay03}
          alt="通过 hatch-pet 生成宠物"
          caption="通过 hatch-pet 生成宠物"
        />
        <Figure
          src={HELP_IMAGES.codexPlay04}
          alt="选择新生成的宠物"
          caption="在外观列表中选择新生成的宠物"
        />
      </section>

      <section className="help-section">
        <h2>4. HachimoDock 里怎么导入</h2>
        <ol className="help-list">
          <li>点击“从社区导入”旁边的问号，先了解来源和目录规则。</li>
          <li>需要现成安装命令时，可把社区链接、curl 命令或 `npx codex-pets add ...` 粘贴进导入框。</li>
          <li>如果你已经在本机安装好了资源，也可以直接重新扫描。</li>
        </ol>
        <Figure
          src={HELP_IMAGES.importEntry}
          alt="HachimoDock 中的导入入口"
          caption="HachimoDock 中的导入入口"
        />
      </section>

      <section className="help-section">
        <h2>5. 默认目录与排查</h2>
        <p>Windows 上常见的默认目录是：</p>
        <div className="help-code">C:\Users\&lt;你的用户名&gt;\.codex\pets</div>
        <p>
          如果导入不到，先确认宠物资源是否真的在这个目录下，再回到 HachimoDock 重新扫描。
        </p>
        <Figure
          src={HELP_IMAGES.petsFolder}
          alt="查看 pets 文件夹位置"
          caption="先确认本机 pets 文件夹位置，再进行导入"
        />
      </section>

      <section className="help-section">
        <h2>6. 常见问题</h2>
        <div className="help-faq">
          <strong>为什么详情页看起来不一样？</strong>
          <p>
            自建视频形象和 codex pet 走的是不同素材链路，只要资源能被正确扫描和预览，详情页长相不同是正常的。
          </p>
        </div>
        <div className="help-faq">
          <strong>为什么社区里有，管理端里没有？</strong>
          <p>
            常见原因是资源还没真正装进本机目录、目录层级不符合要求，或者导入后没有重新扫描。
          </p>
        </div>
      </section>
    </div>
  );
}
