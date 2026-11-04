/** Shared generation and visual-review policy for every desktop platform. */
export const SUBJECT_MOTION_POLICY_ZH = "主体可以是任意外观形态，包括无手臂、无腿、无头、无五官的机器人、物体、几何体或抽象角色。只使用参考图实际存在的结构；不按人物、动物或机器人分类推定必须具有的器官。以动作含义和可辨认的状态变化为准，可使用整体倾斜、平移、弹跳、旋转、节奏起伏或符合原材质的自然压缩、舒展与形变。不要求抬手、招手、微笑、眨眼、肩膀或关节，不能因缺少这些器官而拒绝。左右朝向与角度仅是可适配示例，只有方向本身就是状态含义时才要求对应方向。完整主体指参考图中设计好的完整外形，不要求补成人形全身；保留原有部件与配饰，不凭空添加肢体、五官或物件。";
export const SUBJECT_MOTION_POLICY_EN = "Accept any subject morphology, including armless, legless, headless, faceless, robotic, object-like, geometric and abstract forms. Use only structures actually present in the reference, never anatomy inferred from a category. Judge the readable meaning of the state; whole-form tilt, translation, bounce, rotation, rhythmic motion or material-appropriate squash, stretch and deformation are valid equivalents. Arms, waving hands, shoulders, joints, smiles, eyes and blinking are never mandatory. Left/right and exact angles are adaptable examples unless direction itself is the semantic purpose of the state. The complete subject means its original designed outline, not a mandatory humanoid body. Preserve existing parts and accessories; never invent limbs, facial features or props.";

export function withSubjectMotionPolicy(prompt) {
 const text=String(prompt||'');
 return text.includes(SUBJECT_MOTION_POLICY_EN)?text:text+'\nApply this anatomy-adaptive rule to any earlier pose examples or cached instructions: '+SUBJECT_MOTION_POLICY_EN;
}
