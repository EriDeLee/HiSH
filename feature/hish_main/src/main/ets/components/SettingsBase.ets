@Component
export struct BlockTitle {
  content?: string | Resource

  build() {
    Text(this.content)
      .fontSize($r('sys.float.Body_M'))
      .fontColor($r('sys.color.font_secondary'))
      .margin({ bottom: 8, left: 16 })
      .width('100%')
  }
}

export class SettingBlockStyle implements AttributeModifier<ColumnAttribute> {
  applyNormalAttribute(instance: ColumnAttribute): void {
    instance.borderRadius(16)
      .backgroundColor($r('app.color.setting_block_background'))
      .margin({ bottom: 24 })
  }
}

@Component
export struct ItemTitle {
  @Prop title: string | Resource
  @Prop subTitle: string | Resource

  build() {
    Column({ space: 4 }) {
      Text(this.title).fontSize($r('sys.float.Body_L')).fontColor($r('sys.color.font_primary'))
      Text(this.subTitle).fontSize($r('sys.float.Body_M')).fontColor($r('sys.color.font_secondary'))
    }
    .alignItems(HorizontalAlign.Start)
  }
}

@Component
export struct SettingItem {
  @Prop title: string | Resource
  @Prop subTitle: string | Resource
  @Prop icon: Resource | undefined = undefined
  @BuilderParam content?: () => void
  @Prop stacked: boolean = false

  build() {

    Flex({ direction: this.stacked ? FlexDirection.Column : FlexDirection.Row,
      alignItems: this.stacked ? ItemAlign.Start : ItemAlign.Center }) {
      ItemTitle({
        title: this.title,
        subTitle: this.subTitle
      })
        .layoutWeight(this.stacked ? 0 : 1)
        .width(this.stacked ? '100%' : undefined)
        .margin({ bottom: this.stacked ? 8 : 0 })

      if (this.content) {
        this.content()
      }
      if (this.icon) {
        SymbolGlyph(this.icon)
          .fontColor([$r('sys.color.icon_secondary')])
          .fontSize('24vp')
      }
    }
    .width('100%')
    .constraintSize({ minHeight: 56 })
    .clickEffect({ level: ClickEffectLevel.MIDDLE })
    .padding(16)
  }
}

@Component
export struct LinkItem {
  @Prop icon: Resource | undefined = undefined
  @Prop title: string | Resource
  @Prop subTitle: string | Resource
  click?: () => void

  build() {
    Row() {
      if (this.icon) {
        SymbolGlyph(this.icon)
          .fontSize('24vp')
          .margin({ right: $r('sys.float.padding_level8') })
          .fontColor([$r('sys.color.icon_emphasize')])
      }
      Column() {
        Text(this.title).fontSize($r('sys.float.Body_L')).fontColor($r('sys.color.font_primary'))
        Text(this.subTitle).fontSize($r('sys.float.Body_M')).fontColor($r('sys.color.font_tertiary'))
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      if (this.click) {
        SymbolGlyph($r('sys.symbol.chevron_right'))
          .fontColor([$r('sys.color.icon_secondary')])
          .fontSize('24vp')
      }
    }
    .onClick(() => {
      if (this.click) {
        this.click();
      }
    })
    .clickEffect({ level: ClickEffectLevel.MIDDLE })
    .alignItems(VerticalAlign.Center)
    .width('100%')
    .constraintSize({ minHeight: 56 })
    .padding(16)
  }
}
