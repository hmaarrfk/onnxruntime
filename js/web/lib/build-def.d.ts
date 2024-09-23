// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

/* eslint-disable @typescript-eslint/naming-convention */

/**
 * The interface BuildDefinitions contains a set of flags which are defined at build time.
 *
 * Those flags are processed in bundler for tree shaking to remove unused code.
 * No flags in this file should present in production build.
 */
interface BuildDefinitions {
  // #region Build definitions for Tree Shaking

  /**
   * defines whether to disable the whole WebGL backend in the build.
   */
  readonly DISABLE_WEBGL: boolean;
  /**
   * defines whether to disable the whole WebGpu/WebNN backend in the build.
   */
  readonly DISABLE_JSEP: boolean;
  /**
   * defines whether to disable the whole WebNN backend in the build.
   */
  readonly DISABLE_WASM: boolean;
  /**
   * defines whether to disable proxy feature in WebAssembly backend in the build.
   */
  readonly DISABLE_WASM_PROXY: boolean;
  /**
   * defines whether to disable dynamic importing WASM module in the build.
   */
  readonly DISABLE_DYNAMIC_IMPORT: boolean;

  // #endregion

  // #region Build definitions for ESM

  /**
   * defines whether the build is ESM.
   */
  readonly IS_ESM: boolean;
  /**
   * placeholder for the import.meta.url in ESM. in CJS, this is undefined.
   */
  readonly ESM_IMPORT_META_URL: string | undefined;

  // #endregion

  /**
   * placeholder for the bundle filename.
   *
   * This is used for bundler compatibility fix when using Webpack with `import.meta.url` inside ESM module.
   *
   * The default behavior of some bundlers (eg. Webpack) is to rewrite `import.meta.url` to the file local path at
   * compile time. This behavior will break the following code:
   * ```js
   * new Worker(new URL(import.meta.url), { type: 'module' });
   * ```
   *
   * This is because the `import.meta.url` will be rewritten to a local path, so the line above will be equivalent to:
   * ```js
   * new Worker(new URL('file:///path/to/your/file.js'), { type: 'module' });
   * ```
   *
   * This will cause the browser fails to load the worker script.
   *
   * To fix this, we need to align with how the bundlers deal with `import.meta.url`:
   * ```js
   * new Worker(new URL('path-to-bundle.mjs', import.meta.url), { type: 'module' });
   * ```
   *
   * This will make the browser load the worker script correctly.
   *
   * Since we have multiple bundle outputs, we need to define this placeholder in the build definitions.
   */
  readonly BUNDLE_FILENAME: string;
}

declare const BUILD_DEFS: BuildDefinitions;
