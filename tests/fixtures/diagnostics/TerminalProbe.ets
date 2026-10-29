// Retained diagnostic UI hooks. Deliberately outside every application module.
// Restoring a diagnostic UI requires an explicit source integration and build;
// HISH_ENABLE_DIAGNOSTICS alone enables only the native diagnostic targets.
import { probe as runNativeProbe, diagnosticsEnabled } from 'libhish_main.so';
import { Want } from '@kit.AbilityKit';

export function recordProbeLaunch(want: Want): void {
  AppStorage.setOrCreate('nativeProbe', diagnosticsEnabled() && typeof want.parameters?.nativeProbe === 'string' ? want.parameters.nativeProbe : '');
}

export function probeMenu(start: (mode: string) => void): MenuElement[] {
  if (!diagnosticsEnabled()) { return []; }
  return [{ value: '检测原生能力', action: () => { start('probe'); } }];
}

export async function launchProbe(root: string, writeStatus: (message: string) => void): Promise<boolean> {
  const probe = diagnosticsEnabled() ? AppStorage.get<string>('nativeProbe') ?? '' : '';
  if (probe.length === 0) { return false; }
  AppStorage.set('nativeProbe', '');
  writeStatus('独立启动的原生探针: ' + probe);
  try {
    const pending: Promise<string> = runNativeProbe(root, probe);
    const report = await pending;
    writeStatus(report);
  } catch (err) {
    writeStatus('探针失败: ' + (err as Error).message);
  }
  return true;
}
