// =============================================================================
//  input.js —— 输入采集
//
//  .io 品类的标准手感：角色朝鼠标指针方向移动。
//  同时保留 WASD / 方向键作为备用（触摸板或单手操作更顺手）。
// =============================================================================

import { CONFIG } from './config.js';

export class InputController {
  constructor(canvas) {
    this.canvas = canvas;
    this.keys = new Set();

    // 鼠标在"屏幕坐标系"下的位置
    this.pointer = { x: 0, y: 0 };
    this.pointerInside = false;
    this.pointerActive = false;
    this.pointerType = 'mouse';

    this.bindEvents();
  }

  bindEvents() {
    window.addEventListener('keydown', (e) => {
      this.keys.add(e.code);
      // 避免空格滚动页面
      if (e.code === 'Space') e.preventDefault();
    });

    window.addEventListener('keyup', (e) => {
      this.keys.delete(e.code);
    });

    window.addEventListener('blur', () => {
      this.keys.clear();
      this.pointerActive = false;
    });

    this.canvas.addEventListener('pointerdown', (e) => {
      this.pointerType = e.pointerType || 'mouse';
      this.pointerActive = true;
      this.pointerInside = true;
      this.pointer.x = e.clientX;
      this.pointer.y = e.clientY;
      this.canvas.setPointerCapture?.(e.pointerId);
    });

    this.canvas.addEventListener('pointermove', (e) => {
      if (this.pointerType !== 'mouse' && !this.pointerActive) {
        return;
      }
      this.pointer.x = e.clientX;
      this.pointer.y = e.clientY;
      this.pointerInside = true;
    });

    this.canvas.addEventListener('pointerup', () => {
      this.pointerActive = false;
      if (this.pointerType !== 'mouse') {
        this.pointerInside = false;
      }
    });

    this.canvas.addEventListener('pointercancel', () => {
      this.pointerActive = false;
      this.pointerInside = false;
    });

    this.canvas.addEventListener('pointerleave', () => {
      if (this.pointerType === 'mouse' && !this.pointerActive) {
        this.pointerInside = false;
      }
    });
  }

  /**
   * 计算当前移动方向（单位向量，长度 0 或 1）。
   * @param {{x:number,y:number}} selfScreen 自己角色在屏幕上的位置
   * @param {number} deadZone 鼠标死区（屏幕像素），避免抖动
   */
  getDirection(selfScreen, deadZone = 12) {
    if (this.keys.has('Space')) {
      return { x: 0, y: 0 };
    }

    // 键盘优先（有输入时用键盘，避免鼠标停在原地导致漂移）
    let kx = 0;
    let ky = 0;
    if (this.keys.has('KeyW') || this.keys.has('ArrowUp')) ky -= 1;
    if (this.keys.has('KeyS') || this.keys.has('ArrowDown')) ky += 1;
    if (this.keys.has('KeyA') || this.keys.has('ArrowLeft')) kx -= 1;
    if (this.keys.has('KeyD') || this.keys.has('ArrowRight')) kx += 1;

    if (kx !== 0 || ky !== 0) {
      const len = Math.hypot(kx, ky);
      return { x: kx / len, y: ky / len };
    }

    if (!this.pointerInside) {
      return { x: 0, y: 0 };
    }

    const dx = this.pointer.x - selfScreen.x;
    const dy = this.pointer.y - selfScreen.y;
    const dist = Math.hypot(dx, dy);
    if (dist < deadZone) {
      return { x: 0, y: 0 };
    }

    // 方向输入不需要按距离缩放（距离只影响是否移动）
    return { x: dx / dist, y: dy / dist };
  }

  /** 输入发送节流：按 CONFIG.inputHz 控制上报频率 */
  shouldSend(nowMs, lastSendMs) {
    return nowMs - lastSendMs >= 1000 / CONFIG.inputHz;
  }
}
