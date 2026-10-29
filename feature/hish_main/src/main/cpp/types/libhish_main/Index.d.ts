export interface NativeSessionInfo {
  id: number;
  pid: number;
  pty: boolean;
  ptyError: number;
}
export interface NativeOutput {
  data: ArrayBuffer;
  running: boolean;
  rawInput: boolean;
  echoInput: boolean;
  convertEol: boolean;
  controlChars: ArrayBuffer;
  localFlags: number;
}
export const start: (root: string, columns: number, rows: number, mode: string) => Promise<NativeSessionInfo>;
export const read: (id: number) => NativeOutput;
export const write: (id: number, data: ArrayBuffer) => number;
export const resize: (id: number, columns: number, rows: number) => void;
export const close: (id: number) => void;
export const probe: (root: string, kind: string) => Promise<string>;
export const diagnosticsEnabled: () => boolean;
