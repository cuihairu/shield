# Schema Mapper

> Status: deferred extension design.
>
> The current Shield refactor keeps only raw DB / Redis access in core. Schema
> mapper is not part of the runtime core contract and must be treated as a later
> optional extension unless the roadmap explicitly changes.

Schema mapper 是 Shield 契约系统中的数据访问定义层。它参考 MyBatis 的显式 mapper 思路，用 XML 描述 SQL、参数绑定、结果映射和事务边界，但不把 Shield 变成重 ORM。

## 设计目标

1. 探索数据库访问的可选扩展形态，不进入当前 runtime core。
2. 通过 XML 契约生成服务端 mapper 接口和运行时元数据。
3. 显式 SQL 优先，避免隐式查询和对象图级联。
4. 共享 schema 类型系统，但不把 RPC DTO 等同于数据库 entity。
5. mapper 只进入服务端 descriptor，默认不下发客户端。
6. 第一版支持关系型数据库，后续再扩展 Redis、MongoDB 等后端。

## 边界

mapper 负责：

- SQL statement 定义
- 参数绑定
- 结果映射
- 事务策略
- 分页约束
- 批量操作
- 可选缓存提示
- 生成服务端接口

mapper 不负责：

- 客户端协议
- 服务路由
- 业务流程编排
- 自动对象图加载
- ActiveRecord 生命周期
- 跨服务分布式事务

## 文件位置

mapper 文件放在业务模块目录下：

```text
protocol/
  player/
    types.xml
    services.xml
    mappers.xml
```

一个模块可以拆多个 mapper 文件。manifest 可以支持显式 include：

```xml
<module name="player" path="player">
  <types file="types.xml"/>
  <services file="services.xml"/>
  <mappers file="mappers.xml"/>
  <mappers file="inventory_mappers.xml"/>
</module>
```

## Entity 和 DTO

mapper 可以定义 `entity`，但 entity 是持久化模型，不等同于 RPC DTO。

```xml
<entity name="PlayerEntity" table="player">
  <field name="player_id" column="player_id" type="string" id="1" primaryKey="true"/>
  <field name="nickname" column="nickname" type="string" id="2"/>
  <field name="level" column="level" type="int32" id="3"/>
  <field name="created_at" column="created_at" type="int64" id="4"/>
  <field name="updated_at" column="updated_at" type="int64" id="5"/>
</entity>
```

RPC 返回结构应该使用 DTO 或 view：

```xml
<struct name="PlayerProfile">
  <field name="player_id" id="1" type="string"/>
  <field name="nickname" id="2" type="string"/>
  <field name="level" id="3" type="int32"/>
</struct>
```

这可以避免数据库字段变化直接污染客户端协议。

公共类型、entity/view 边界和校验规则见 [Schema Types](schema-types.md)。

## Mapper 定义

```xml
<mappers namespace="player">
  <mapper name="PlayerMapper" id="100">
    <select name="SelectProfile"
            id="1"
            paramType="player.GetProfileRequest"
            resultType="player.PlayerProfile"
            timeout_ms="1000">
      SELECT player_id, nickname, level
      FROM player
      WHERE player_id = #{player_id}
    </select>

    <update name="UpdateNickname"
            id="2"
            paramType="player.UpdateNicknameRequest"
            resultType="common.AffectedRows">
      UPDATE player
      SET nickname = #{nickname}
      WHERE player_id = #{player_id}
    </update>
  </mapper>
</mappers>
```

规则：

- `mapper id` 在模块内唯一。
- `statement id` 在 mapper 内唯一。
- 发布后的 ID 不能复用。
- `paramType` 必须引用 schema type。
- `resultType` 或 `resultMap` 必须显式声明。
- SQL 中只能使用声明式参数绑定，不允许字符串拼接参数。

## Statement 类型

第一版支持：

- `select`
- `insert`
- `update`
- `delete`

后续可以扩展：

- `batchInsert`
- `batchUpdate`
- `upsert`
- `callProcedure`

## 参数绑定

参数使用 `#{name}` 绑定，表示安全 prepared statement 参数。

```sql
WHERE player_id = #{player_id}
```

可选支持 `${name}` 作为原样替换，但第一版不建议开放。如果必须支持，只能用于经过白名单校验的标识符场景，例如排序字段：

```xml
<bind name="order_by" source="order_by" mode="identifier" allow="level,nickname,created_at"/>
```

建议第一版只实现 `#{}`，不实现 `${}`。

## 嵌套参数

支持点路径：

```sql
WHERE guild_id = #{filter.guild_id}
  AND level >= #{filter.min_level}
```

路径必须能在 `paramType` 中静态解析，编译期校验失败则拒绝生成 descriptor。

## Result Type

简单结果可以直接用 `resultType`：

```xml
<select name="SelectProfile"
        id="1"
        paramType="player.GetProfileRequest"
        resultType="player.PlayerProfile">
  SELECT player_id, nickname, level
  FROM player
  WHERE player_id = #{player_id}
</select>
```

字段名默认按 column name 匹配 struct field name。建议允许 `snake_case` 到 `camelCase` 映射，但必须可配置。

## Result Map

复杂映射使用 `resultMap`：

```xml
<resultMap name="PlayerProfileMap" type="player.PlayerProfile">
  <result column="player_id" property="player_id"/>
  <result column="nickname" property="nickname"/>
  <result column="level" property="level"/>
</resultMap>

<select name="SelectProfile"
        id="1"
        paramType="player.GetProfileRequest"
        resultMap="PlayerProfileMap">
  SELECT p.player_id, p.nickname, p.level
  FROM player p
  WHERE p.player_id = #{player_id}
</select>
```

第一版 resultMap 不做自动关联加载。需要 join 时，用显式 SQL 返回扁平 DTO。

## 返回形态

`select` 默认返回单条。列表查询显式声明：

```xml
<select name="ListPlayers"
        id="2"
        paramType="player.ListPlayersRequest"
        resultType="list<player.PlayerProfile>"
        maxRows="100">
  SELECT player_id, nickname, level
  FROM player
  WHERE level >= #{min_level}
  ORDER BY level DESC
  LIMIT #{limit}
</select>
```

规则：

- 列表查询必须有 `maxRows`。
- 分页查询必须有上限。
- 没有 `WHERE` 的 `update/delete` 默认拒绝，除非显式 `allowFullTable="true"`。

## 事务

第一版支持 statement 级事务提示，但一个 mapper statement 只能包含一条 SQL。多步事务由 service 层显式编排。

```xml
<update name="DebitGold"
        id="10"
        paramType="player.DebitGoldRequest"
        resultType="common.AffectedRows"
        transaction="required">
  UPDATE wallet
  SET gold = gold - #{amount}
  WHERE player_id = #{player_id}
    AND gold >= #{amount}
</update>
```

事务策略：

- `none`: 不主动开启事务。
- `required`: 没有事务则开启，有则复用。
- `requires_new`: 总是开启新事务。

不建议第一版做跨 mapper 自动事务编排。跨多步业务事务应该由 service 层显式控制：

```lua
shield.db.transaction(function(tx)
  shield.db.PlayerMapper:DebitGold(tx, { player_id = from_id, amount = amount })
  shield.db.PlayerMapper:CreditGold(tx, { player_id = to_id, amount = amount })
end)
```

禁止在单个 mapper statement 中编写多条 SQL，避免 prepared statement 行为差异、结果映射复杂化和驱动兼容问题。

## 动态 SQL

动态 SQL 很实用，但也是复杂度来源。第一版只建议支持最小集合：

```xml
<select name="SearchPlayers"
        id="3"
        paramType="player.SearchPlayersRequest"
        resultType="list<player.PlayerProfile>"
        maxRows="100">
  SELECT player_id, nickname, level
  FROM player
  <where>
    <if test="nickname != null">
      nickname LIKE #{nickname}
    </if>
    <if test="min_level != null">
      AND level >= #{min_level}
    </if>
  </where>
</select>
```

建议第一版支持：

- `if`
- `where`
- `set`
- `foreach` 用于 `IN` 列表

暂不支持：

- 任意表达式语言
- include fragment
- provider method
- 用户自定义 SQL 节点

表达式语言必须受限，不能执行脚本。

## 分页

分页应该由契约显式表达：

```xml
<select name="ListPlayers"
        id="4"
        paramType="common.PageRequest"
        resultType="common.PageResult<player.PlayerProfile>"
        paging="offset"
        maxRows="100">
  SELECT player_id, nickname, level
  FROM player
  ORDER BY player_id
  LIMIT #{limit} OFFSET #{offset}
</select>
```

后续可支持 cursor paging：

```xml
<select name="ListPlayersByCursor"
        id="5"
        paramType="common.CursorRequest"
        resultType="common.CursorResult<player.PlayerProfile>"
        paging="cursor">
  SELECT player_id, nickname, level
  FROM player
  WHERE player_id > #{cursor}
  ORDER BY player_id
  LIMIT #{limit}
</select>
```

## 缓存提示

缓存先作为 hint，不作为强制语义：

```xml
<select name="SelectProfile"
        id="1"
        paramType="player.GetProfileRequest"
        resultType="player.PlayerProfile"
        cache="local"
        cacheKey="player:{player_id}"
        ttl_ms="30000">
  SELECT player_id, nickname, level
  FROM player
  WHERE player_id = #{player_id}
</select>
```

缓存策略建议：

- `none`
- `local`
- `redis`

第一版可以只生成元数据，不实现复杂一致性策略。写操作后的缓存失效必须显式声明：

```xml
<update name="UpdateNickname"
        id="2"
        paramType="player.UpdateNicknameRequest"
        resultType="common.AffectedRows"
        invalidateCache="player:{player_id}">
  UPDATE player SET nickname = #{nickname}
  WHERE player_id = #{player_id}
</update>
```

## 数据库方言

SQL 方言需要显式声明：

```xml
<mappers namespace="player" dialect="mysql">
```

可选值：

- `mysql`
- `postgres`
- `sqlite`

第一版可以先支持 `mysql` 和 `sqlite`，因为本地开发和生产路径都容易覆盖。

同一个 statement 可以提供多个方言版本：

```xml
<select name="Now" id="20" resultType="common.Timestamp">
  <sql dialect="mysql">SELECT UNIX_TIMESTAMP() AS value</sql>
  <sql dialect="sqlite">SELECT strftime('%s','now') AS value</sql>
</select>
```

## Generated Server API

生成器应输出服务端 mapper 接口。

C++ 示例：

```cpp
class PlayerMapper {
public:
    task<PlayerProfile> SelectProfile(const GetProfileRequest& req);
    task<AffectedRows> UpdateNickname(const UpdateNicknameRequest& req);
};
```

Lua 示例：

```lua
local profile = shield.db.PlayerMapper:SelectProfile({ player_id = player_id })
```

生成代码只做类型和调用体验，真正执行仍由 mapper runtime 按 descriptor 处理。

## Runtime Flow

```text
service handler
  -> generated mapper facade
  -> mapper runtime
  -> resolve statement descriptor
  -> bind parameters
  -> prepare statement
  -> execute
  -> map result rows
  -> return typed result or ShieldError
```

mapper runtime 必须记录：

- mapper name
- statement name
- SQL hash
- duration
- affected rows
- row count
- error code

这些数据进入日志和 metrics。

## Descriptor Profile

mapper 默认只进入 server profile：

```text
descriptor.server.bin
  types
  services
  errors
  mappers

descriptor.client.bin
  types exposed to client
  services exposed to client
  errors exposed to client
```

客户端不应收到 SQL、表名、列名、缓存 key 或事务策略。

## 安全规则

编译期必须校验：

- SQL 参数都能在 `paramType` 中解析。
- `resultType` 字段能被查询列覆盖，除非字段 optional。
- `update/delete` 默认必须有 `WHERE`。
- 列表查询必须有 `maxRows`。
- 动态 SQL 表达式不能执行任意脚本。
- 不允许未声明的原样字符串替换。

运行期必须限制：

- statement timeout
- 最大返回行数
- 最大参数数量
- 最大 SQL 长度
- 连接池资源

## Phase 1 范围

建议第一版 mapper 只做：

- `select/insert/update/delete`
- `#{}` 参数绑定
- `resultType`
- 简单 `resultMap`
- statement timeout
- `maxRows`
- `transaction=required`
- server-only descriptor profile
- C++/Lua mapper facade
- 单 statement 单 SQL

推迟：

- 复杂动态 SQL
- 多方言 statement
- 分布式事务
- mapper 多语句脚本
- 自动关联加载
- 二级缓存一致性
- schema migration

## Open Decisions

- mapper runtime 第一版绑定哪个数据库抽象：现有 database 模块，还是新增 lightweight DB runtime。
- `entity` 是否生成 schema migration 草案。

已定规则：

- Lua mapper facade 不能阻塞 actor 线程，表面同步 API 应基于 coroutine/yield，另提供 async 回调 API。
- SQL 第一版不允许多语句。
- mapper 只能作为后续可选扩展讨论，不进入当前最小启动路径。
