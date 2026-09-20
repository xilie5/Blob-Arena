import { defineConfig } from 'vite';

/**
 * Vite 配置
 *
 * 目的：
 *  - 在不污染 C++ 后端的前提下，给 web/ 提供现代开发体验（HMR、ESM、WS 反代）。
 *  - 通过 server.proxy 把前端发出的 /ws 请求转发到真正的 C++ 后端 (端口 8080)，
 *    统一解决开发端口与 C++ 后端的跨域和 WebSocket 代理问题。
 *
 * 开发命令：npm run dev    （默认 http://localhost:5173）
 * 构建产物：npm run build  （输出到 dist/，可直接拷给生产静态托管）
 */
export default defineConfig({
  // 整个 web/ 作为 Vite 根目录，里面已经有 index.html / css / js / assets
  root: 'web',

  // Vite 5 缺省开启 HMR 用的 WS 是同源的（端口 5173），不需要单独暴露；
  // 这里只把业务用的 /ws 反代到 C++ 后端即可。
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: false,
    // 让 Vite 在源文件变化时刷新整个页面而不是模块热替换 —— Canvas 渲染不
    // 适合做局部 HMR，刷新更省心
    hmr: { overlay: true },
    proxy: {
      // 客户端仍然连 ws://localhost:5173/ws，Vite 在这里转给真正的后端。
      // ws: true 保留 WebSocket 升级；changeOrigin 重写 Host 头避免后端校验失败。
      '/ws': {
        target: 'ws://127.0.0.1:8080',
        ws: true,
        changeOrigin: true,
        rewrite: (path) => path, // 不改路径，原样 /ws → /ws
      },
      // 兜底：万一后端加了其他 HTTP 端点（REST 调试、健康检查等），也走同源
      '/api': {
        target: 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
    },
  },

  build: {
    // 生产构建产物目录（相对 vite root 即 web/）
    outDir: 'dist',
    emptyOutDir: true,
    // Canvas / 静态资源不需要哈希，方便后端直接服务
    assetsDir: 'assets',
    target: 'es2020',
    sourcemap: true,
  },

  // 允许 web/ 下子目录作为静态资源被引用（icons / 字体等不会被 Vite 改写）
  publicDir: false,
});
