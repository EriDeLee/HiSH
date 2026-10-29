import { childProcessManager } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError, deviceInfo } from '@kit.BasicServicesKit';
import AuditArkChild from './AuditArkChild';

// Invoked only for an explicit test launch; ordinary terminal startup is unchanged.
export async function auditProcessRoutes(): Promise<string> {
  let report = '';
  const record = (line: string): void => {
    report += line + '\n';
    hilog.info(0, 'HiSHProbe', '%{public}s', line);
  };
  record('PROBE framework-ark BEGIN');
  if (deviceInfo.apiAvailable('26.0.0')) {
    record('ark supported API26=' + childProcessManager.isArkChildProcessSupported());
    record('native supported ArkTS API26=' + childProcessManager.isNativeChildProcessSupported());
  }
  AuditArkChild.toString();
  for (const mode of [childProcessManager.StartMode.SELF_FORK, childProcessManager.StartMode.APP_SPAWN_FORK]) {
    try {
      const pid = await childProcessManager.startChildProcess('./ets/native/AuditArkChild.ets', mode);
      record('startChildProcess mode=' + mode + ' pid=' + pid);
    } catch (error) {
      record('startChildProcess mode=' + mode + ' code=' + (error as BusinessError).code);
    }
  }
  try {
    const pid = await childProcessManager.startArkChildProcess('./ets/native/AuditArkChild.ets', { entryParams: 'owned-audit' });
    record('startArkChildProcess pid=' + pid);
  } catch (error) {
    record('startArkChildProcess code=' + (error as BusinessError).code);
  }
  try {
    const pid = await childProcessManager.startNativeChildProcess('libhish_probe_payload.so:HiSHRouteChild', { entryParams: 'owned-audit' });
    record('startNativeChildProcess pid=' + pid);
  } catch (error) {
    record('startNativeChildProcess code=' + (error as BusinessError).code);
  }
  record('PROBE framework-ark END');
  return report;
}
