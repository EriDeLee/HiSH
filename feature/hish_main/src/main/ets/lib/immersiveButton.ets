// 保留系统默认按钮样式及现有修饰器接口。target 为 API 26，最低兼容 API 24；
// SDK 升级不在这里引入新的材质效果。

/**
 * Button 样式修饰器，保持系统默认样式。
 */
export class ImmersiveButtonModifier implements AttributeModifier<ButtonAttribute> {
  applyNormalAttribute(instance: ButtonAttribute): void {
    // 保持系统默认样式。
  }
}

/**
 * Toggle(Button 型) 沉浸光感修饰器，同上。
 */
export class ImmersiveToggleModifier implements AttributeModifier<ToggleAttribute> {
  applyNormalAttribute(instance: ToggleAttribute): void {
    // 保持系统默认样式。
  }
}

/**
 * 主按钮（强调色）沉浸光感修饰器，同上。
 */
export class ImmersivePrimaryButtonModifier implements AttributeModifier<ButtonAttribute> {
  applyNormalAttribute(instance: ButtonAttribute): void {
    // 保持系统默认样式。
  }
}
