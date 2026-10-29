import { ChildProcess } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

export default class AuditArkChild extends ChildProcess {
  onStart(): void {
    hilog.info(0, 'HiSHProbe', 'ARK_CHILD_ENTERED');
  }
}
