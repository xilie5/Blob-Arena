// =============================================================================
//  render.js —— Canvas 渲染
//
//  渲染层次（从下到上）：
//    背景 -> 世界网格 -> 世界边界 -> 缩圈危险区阴影 -> 实体（小到大）
//      -> 缩圈描边 -> 昵称 -> 自己高亮环
//
//  相机跟随自己角色，缩放随体积增大而拉远（.io 品类标准手感）。
// =============================================================================

import { CONFIG, PALETTE, EntityKind } from './config.js';

export class Renderer {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d', { alpha: false });
    this.camera = { x: 0, y: 0, zoom: 1 };
    this.width = 0;
    this.height = 0;
    this.dpr = 1;

    // 缩圈参数。服务端每跳秒广播一次，客户端用短暂有效期平滑保留。
    this.shrinkCenter = { x: 0, y: 0 };
    this.shrinkRadius = 0;
    this.shrinkActiveUntilMs = 0;

    this.resize();
    window.addEventListener('resize', () => this.resize());
  }

  /**
   * 由 game.js 在收到 MatchState 时调用。
   * @param {{x:number, y:number, r:number, active:boolean}} info
   */
  setShrinkInfo(info) {
    if (info == null || !info.active) {
      this.shrinkActiveUntilMs = 0;
      this.shrinkRadius = 0;
      return;
    }
    this.shrinkCenter.x = info.x;
    this.shrinkCenter.y = info.y;
    this.shrinkRadius = info.r;
    // 服务端每跳秒广播一次，客户端保留短暂有效期以平滑显示。
    this.shrinkActiveUntilMs = performance.now() + 1100;
  }

  resize() {
    this.dpr = window.devicePixelRatio || 1;
    this.width = window.innerWidth;
    this.height = window.innerHeight;
    this.canvas.width = Math.floor(this.width * this.dpr);
    this.canvas.height = Math.floor(this.height * this.dpr);
  }

  worldToScreen(x, y) {
    return {
      x: (x - this.camera.x) * this.camera.zoom + this.width / 2,
      y: (y - this.camera.y) * this.camera.zoom + this.height / 2,
    };
  }

  computeZoom(radius) {
    const r = Math.max(radius || CONFIG.zoomBaseRadius, CONFIG.zoomBaseRadius);
    const base = Math.pow(CONFIG.zoomBaseRadius / r, CONFIG.zoomExponent);
    const scale = base * Math.min(1.6, Math.max(0.7, this.height / 900));
    return Math.min(CONFIG.zoomMax, Math.max(CONFIG.zoomMin, scale));
  }

  updateCamera(self, lerp = CONFIG.cameraLerp) {
    if (!self) {
      return;
    }
    const targetZoom = this.computeZoom(self.radius);
    this.camera.x += (self.rx - this.camera.x) * lerp;
    this.camera.y += (self.ry - this.camera.y) * lerp;
    this.camera.zoom += (targetZoom - this.camera.zoom) * lerp;
  }

  /** 开局时把相机直接吸附到角色，避免从 (0,0) 飞过去 */
  snapCamera(self) {
    if (!self) {
      return;
    }
    this.camera.x = self.rx;
    this.camera.y = self.ry;
    this.camera.zoom = this.computeZoom(self.radius);
  }

  render(world, entities, selfId) {
    const ctx = this.ctx;
    ctx.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);

    ctx.fillStyle = PALETTE.background;
    ctx.fillRect(0, 0, this.width, this.height);

    if (world) {
      this.drawWorld(ctx, world);
    }

    // 缩圈"非安全区"暗影（在实体层之下，避免盖住玩家）
    if (this.shrinkActiveUntilMs > performance.now() && world) {
      this.drawShrinkZone(ctx, world);
    }

    const list = Array.from(entities.values());
    // 小的先画，大的覆盖在上层，视觉层次更清晰
    list.sort((a, b) => a.radius - b.radius);

    for (const entity of list) {
      this.drawEntity(ctx, entity, entity.id === selfId);
    }

    // 缩圈描边（画在实体之上，避免被玩家身体遮挡）
    if (this.shrinkActiveUntilMs > performance.now()) {
      this.drawShrinkRing(ctx);
    }
  }

  /**
   * 圈外危险区暗影（用 compositing 把圈内挖空）。
   */
  drawShrinkZone(ctx, world) {
    const center = this.worldToScreen(this.shrinkCenter.x, this.shrinkCenter.y);
    const rScreen = this.shrinkRadius * this.camera.zoom;

    ctx.save();
    // 先全屏填一层深红
    ctx.fillStyle = PALETTE.shrinkZoneFill;
    ctx.fillRect(0, 0, this.width, this.height);
    // 再用 clip 把圈内挖空（恢复成原画）
    ctx.globalCompositeOperation = 'destination-out';
    ctx.beginPath();
    ctx.arc(center.x, center.y, rScreen, 0, Math.PI * 2);
    ctx.fill();
    ctx.restore();
  }

  /**
   * 缩圈描边（带轻微脉动）。
   */
  drawShrinkRing(ctx) {
    const center = this.worldToScreen(this.shrinkCenter.x, this.shrinkCenter.y);
    const rScreen = this.shrinkRadius * this.camera.zoom;
    const pulse = 0.5 + 0.5 * Math.sin(performance.now() / 220);
    // 按性能预算加 0.35/0.65 透明度脉动强化"毒圈"感。
    ctx.lineWidth = 4;
    ctx.strokeStyle = PALETTE.shrinkRing;
    ctx.globalAlpha = 0.65 + 0.25 * pulse;
    ctx.setLineDash([14, 8]);
    ctx.beginPath();
    ctx.arc(center.x, center.y, rScreen, 0, Math.PI * 2);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.globalAlpha = 1;
  }

  drawWorld(ctx, world) {
    const zoom = this.camera.zoom;
    const halfW = this.width / 2 / zoom;
    const halfH = this.height / 2 / zoom;

    const viewLeft = this.camera.x - halfW;
    const viewRight = this.camera.x + halfW;
    const viewTop = this.camera.y - halfH;
    const viewBottom = this.camera.y + halfH;

    // 世界底色
    const topLeft = this.worldToScreen(0, 0);
    ctx.fillStyle = PALETTE.worldFill;
    ctx.fillRect(topLeft.x, topLeft.y, world.width * zoom, world.height * zoom);

    // 网格
    const step = CONFIG.gridSize;
    ctx.lineWidth = 1;
    ctx.strokeStyle = PALETTE.grid;
    ctx.beginPath();
    for (let x = Math.floor(Math.max(0, viewLeft) / step) * step; x <= Math.min(world.width, viewRight); x += step) {
      const p = this.worldToScreen(x, 0);
      ctx.moveTo(Math.round(p.x) + 0.5, Math.max(0, topLeft.y));
      ctx.lineTo(Math.round(p.x) + 0.5, Math.min(this.height, topLeft.y + world.height * zoom));
    }
    for (let y = Math.floor(Math.max(0, viewTop) / step) * step; y <= Math.min(world.height, viewBottom); y += step) {
      const p = this.worldToScreen(0, y);
      ctx.moveTo(Math.max(0, topLeft.x), Math.round(p.y) + 0.5);
      ctx.lineTo(Math.min(this.width, topLeft.x + world.width * zoom), Math.round(p.y) + 0.5);
    }
    ctx.stroke();

    // 世界边界
    ctx.strokeStyle = PALETTE.worldBorder;
    ctx.lineWidth = 3;
    ctx.strokeRect(topLeft.x, topLeft.y, world.width * zoom, world.height * zoom);
  }

  drawEntity(ctx, entity, isSelf) {
    const screen = this.worldToScreen(entity.rx, entity.ry);
    const screenRadius = Math.max(2, entity.radius * this.camera.zoom);

    // 视口裁剪
    const margin = screenRadius + 40;
    if (screen.x < -margin || screen.y < -margin ||
        screen.x > this.width + margin || screen.y > this.height + margin) {
      return;
    }

    const color = hexColor(entity.color);

    // 食物数量大（上限 600），只画一个实心圆：
    // 省掉每个实体每帧的径向渐变 + 描边（600 × 60fps 的渐变创建开销可观），
    // 而 7px 的小圆点根本看不出高光与描边差异。
    if (entity.kind === EntityKind.FOOD) {
      ctx.beginPath();
      ctx.arc(screen.x, screen.y, screenRadius, 0, Math.PI * 2);
      ctx.fillStyle = color;
      ctx.fill();
      return;
    }

    // 主体
    ctx.beginPath();
    ctx.arc(screen.x, screen.y, screenRadius, 0, Math.PI * 2);
    ctx.fillStyle = color;
    ctx.fill();

    // 内侧高光，让纯色圆看起来更有体积感（统一风格）
    const glow = ctx.createRadialGradient(
      screen.x - screenRadius * 0.3, screen.y - screenRadius * 0.35, screenRadius * 0.1,
      screen.x, screen.y, screenRadius
    );
    glow.addColorStop(0, 'rgba(255,255,255,0.42)');
    glow.addColorStop(0.55, 'rgba(255,255,255,0.06)');
    glow.addColorStop(1, 'rgba(0,0,0,0.20)');
    ctx.fillStyle = glow;
    ctx.fill();

    // 描边
    ctx.lineWidth = isSelf ? 4 : 2;
    ctx.strokeStyle = isSelf ? PALETTE.selfRing : 'rgba(6, 12, 24, 0.55)';
    ctx.stroke();

    // 昵称
    if (entity.name && screenRadius >= CONFIG.minNameRadius) {
      const fontSize = Math.max(11, Math.min(22, screenRadius * 0.52));
      ctx.font = `800 ${fontSize}px "Baloo 2", "Microsoft YaHei", system-ui, sans-serif`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';

      const metrics = ctx.measureText(entity.name);
      const textWidth = metrics.width;
      const padding = 7;
      const boxWidth = textWidth + padding * 2;
      const boxHeight = fontSize + 6;

      // 半透明胶囊底，保证在任何颜色上都能看清
      roundRect(
        ctx,
        screen.x - boxWidth / 2,
        screen.y - screenRadius - boxHeight - 6,
        boxWidth,
        boxHeight,
        boxHeight / 2
      );
      ctx.fillStyle = 'rgba(7, 11, 22, 0.66)';
      ctx.fill();

      ctx.fillStyle = '#ffffff';
      ctx.fillText(
        entity.name,
        screen.x,
        screen.y - screenRadius - 6 - boxHeight / 2 + 1
      );
    }
  }
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

const colorCache = new Map();

/** 0xRRGGBB -> '#rrggbb'，带缓存避免每帧拼字符串 */
export function hexColor(value) {
  const key = value >>> 0;
  let cached = colorCache.get(key);
  if (cached === undefined) {
    cached = `#${key.toString(16).padStart(6, '0')}`;
    colorCache.set(key, cached);
  }
  return cached;
}

function roundRect(ctx, x, y, w, h, r) {
  const radius = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + radius, y);
  ctx.arcTo(x + w, y, x + w, y + h, radius);
  ctx.arcTo(x + w, y + h, x, y + h, radius);
  ctx.arcTo(x, y + h, x, y, radius);
  ctx.arcTo(x, y, x + w, y, radius);
  ctx.closePath();
}

export { EntityKind };
