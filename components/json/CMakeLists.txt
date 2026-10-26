# 兼容旧版 LiveKit 组件使用的 json 依赖名称。
# ESP-IDF 6.1 中实际提供 JSON 实现的组件名称为 cjson，
# 这里保留 json 组件名并公开转发 cjson，避免修改自动生成的 managed_components。
idf_component_register(
    REQUIRES cjson
)
