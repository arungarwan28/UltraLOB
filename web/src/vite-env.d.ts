/// <reference types="vite/client" />

declare module '*.mjs' {
  export interface EmscriptenModuleOptions {
    locateFile?: (path: string) => string;
  }
  const factory: (options?: EmscriptenModuleOptions) => Promise<Record<string, unknown>>;
  export default factory;
}
