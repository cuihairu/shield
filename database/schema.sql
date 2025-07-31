-- Shield游戏数据库表结构
-- 支持MySQL和PostgreSQL

-- =====================================
-- 1. 玩家数据表
-- =====================================

-- 玩家基础信息表
CREATE TABLE IF NOT EXISTS players (
    id BIGINT PRIMARY KEY,
    name VARCHAR(64) NOT NULL,
    level INT DEFAULT 1,
    experience BIGINT DEFAULT 0,
    gold BIGINT DEFAULT 100,
    health INT DEFAULT 100,
    max_health INT DEFAULT 100,
    position_x FLOAT DEFAULT 0,
    position_y FLOAT DEFAULT 0,
    position_z FLOAT DEFAULT 0,
    last_login TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    INDEX idx_name (name),
    INDEX idx_level (level),
    INDEX idx_last_login (last_login)
);

-- 玩家属性扩展表
CREATE TABLE IF NOT EXISTS player_attributes (
    player_id BIGINT,
    attribute_name VARCHAR(32),
    attribute_value TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    PRIMARY KEY (player_id, attribute_name),
    FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE CASCADE,
    INDEX idx_attribute_name (attribute_name)
);

-- 玩家物品表
CREATE TABLE IF NOT EXISTS player_items (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    player_id BIGINT NOT NULL,
    item_id INT NOT NULL,
    item_name VARCHAR(128),
    quantity INT DEFAULT 1,
    durability INT DEFAULT 100,
    enchantments TEXT,  -- JSON格式
    slot_type VARCHAR(32),  -- inventory, equipment, bank
    slot_index INT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE CASCADE,
    INDEX idx_player_items (player_id, slot_type),
    INDEX idx_item_id (item_id)
);

-- 玩家技能表
CREATE TABLE IF NOT EXISTS player_skills (
    player_id BIGINT,
    skill_name VARCHAR(64),
    skill_level INT DEFAULT 1,
    skill_experience BIGINT DEFAULT 0,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    PRIMARY KEY (player_id, skill_name),
    FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE CASCADE,
    INDEX idx_skill_level (skill_level)
);

-- =====================================
-- 2. 公会系统表
-- =====================================

-- 公会表
CREATE TABLE IF NOT EXISTS guilds (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(64) NOT NULL UNIQUE,
    description TEXT,
    leader_id BIGINT,
    level INT DEFAULT 1,
    experience BIGINT DEFAULT 0,
    gold BIGINT DEFAULT 0,
    max_members INT DEFAULT 50,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    FOREIGN KEY (leader_id) REFERENCES players(id),
    INDEX idx_guild_name (name),
    INDEX idx_guild_level (level)
);

-- 公会成员表
CREATE TABLE IF NOT EXISTS guild_members (
    guild_id BIGINT,
    player_id BIGINT,
    rank VARCHAR(32) DEFAULT 'member',
    contribution BIGINT DEFAULT 0,
    joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    PRIMARY KEY (guild_id, player_id),
    FOREIGN KEY (guild_id) REFERENCES guilds(id) ON DELETE CASCADE,
    FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE CASCADE,
    INDEX idx_rank (rank),
    INDEX idx_contribution (contribution)
);

-- =====================================
-- 3. 游戏世界表
-- =====================================

-- 地图区域表
CREATE TABLE IF NOT EXISTS map_regions (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(128) NOT NULL,
    min_x FLOAT, max_x FLOAT,
    min_y FLOAT, max_y FLOAT,
    min_z FLOAT, max_z FLOAT,
    region_type VARCHAR(32), -- safe, pvp, dungeon, etc.
    level_requirement INT DEFAULT 1,
    description TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- NPC表
CREATE TABLE IF NOT EXISTS npcs (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(128) NOT NULL,
    npc_type VARCHAR(64), -- vendor, quest, guard, etc.
    position_x FLOAT DEFAULT 0,
    position_y FLOAT DEFAULT 0,
    position_z FLOAT DEFAULT 0,
    region_id INT,
    level INT DEFAULT 1,
    health INT DEFAULT 100,
    ai_script VARCHAR(256),
    loot_table TEXT, -- JSON格式
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (region_id) REFERENCES map_regions(id),
    INDEX idx_npc_type (npc_type),
    INDEX idx_position (position_x, position_y, position_z)
);

-- =====================================
-- 4. 交易系统表
-- =====================================

-- 拍卖行表
CREATE TABLE IF NOT EXISTS auction_house (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    seller_id BIGINT NOT NULL,
    item_id INT NOT NULL,
    item_name VARCHAR(128),
    quantity INT DEFAULT 1,
    starting_price BIGINT,
    buyout_price BIGINT,
    current_bid BIGINT DEFAULT 0,
    current_bidder_id BIGINT,
    expires_at TIMESTAMP,
    status VARCHAR(32) DEFAULT 'active', -- active, sold, expired, cancelled
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (seller_id) REFERENCES players(id),
    FOREIGN KEY (current_bidder_id) REFERENCES players(id),
    INDEX idx_status (status),
    INDEX idx_expires_at (expires_at),
    INDEX idx_item (item_id, item_name)
);

-- 交易记录表
CREATE TABLE IF NOT EXISTS trade_history (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    seller_id BIGINT,
    buyer_id BIGINT,
    item_id INT,
    item_name VARCHAR(128),
    quantity INT,
    price BIGINT,
    trade_type VARCHAR(32), -- auction, direct, npc
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (seller_id) REFERENCES players(id),
    FOREIGN KEY (buyer_id) REFERENCES players(id),
    INDEX idx_trade_type (trade_type),
    INDEX idx_created_at (created_at),
    INDEX idx_seller (seller_id),
    INDEX idx_buyer (buyer_id)
);

-- =====================================
-- 5. 任务系统表
-- =====================================

-- 任务模板表
CREATE TABLE IF NOT EXISTS quest_templates (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(128) NOT NULL,
    description TEXT,
    quest_type VARCHAR(32), -- kill, collect, deliver, etc.
    level_requirement INT DEFAULT 1,
    prerequisites TEXT, -- JSON格式，前置任务ID
    objectives TEXT, -- JSON格式，任务目标
    rewards TEXT, -- JSON格式，奖励内容
    script_path VARCHAR(256),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    INDEX idx_quest_type (quest_type),
    INDEX idx_level_requirement (level_requirement)
);

-- 玩家任务状态表
CREATE TABLE IF NOT EXISTS player_quests (
    player_id BIGINT,
    quest_id INT,
    status VARCHAR(32) DEFAULT 'in_progress', -- in_progress, completed, failed
    progress TEXT, -- JSON格式，进度数据
    started_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    completed_at TIMESTAMP NULL,
    
    PRIMARY KEY (player_id, quest_id),
    FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE CASCADE,
    FOREIGN KEY (quest_id) REFERENCES quest_templates(id),
    INDEX idx_status (status),
    INDEX idx_started_at (started_at)
);

-- =====================================
-- 6. PvP系统表
-- =====================================

-- PvP排行榜表
CREATE TABLE IF NOT EXISTS pvp_rankings (
    player_id BIGINT PRIMARY KEY,
    rating INT DEFAULT 1000,
    wins INT DEFAULT 0,
    losses INT DEFAULT 0,
    streak INT DEFAULT 0,
    highest_rating INT DEFAULT 1000,
    season_id INT DEFAULT 1,
    last_match_at TIMESTAMP,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE CASCADE,
    INDEX idx_rating (rating DESC),
    INDEX idx_season (season_id, rating DESC)
);

-- PvP比赛记录表
CREATE TABLE IF NOT EXISTS pvp_matches (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    match_type VARCHAR(32), -- duel, arena, battleground
    player1_id BIGINT,
    player2_id BIGINT,
    winner_id BIGINT,
    player1_rating_before INT,
    player1_rating_after INT,
    player2_rating_before INT,
    player2_rating_after INT,
    duration INT, -- 比赛时长（秒）
    season_id INT DEFAULT 1,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (player1_id) REFERENCES players(id),
    FOREIGN KEY (player2_id) REFERENCES players(id),
    FOREIGN KEY (winner_id) REFERENCES players(id),
    INDEX idx_match_type (match_type),
    INDEX idx_created_at (created_at),
    INDEX idx_season (season_id)
);

-- =====================================
-- 7. 日志和统计表
-- =====================================

-- 游戏事件日志表
CREATE TABLE IF NOT EXISTS game_events (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    player_id BIGINT,
    event_type VARCHAR(64) NOT NULL,
    event_data TEXT, -- JSON格式
    server_id VARCHAR(32),
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    FOREIGN KEY (player_id) REFERENCES players(id) ON DELETE SET NULL,
    INDEX idx_event_type (event_type),
    INDEX idx_timestamp (timestamp),
    INDEX idx_player_events (player_id, timestamp),
    
    -- 分区表（按时间分区）
    PARTITION BY RANGE (UNIX_TIMESTAMP(timestamp)) (
        PARTITION p_current VALUES LESS THAN (UNIX_TIMESTAMP('2024-02-01')),
        PARTITION p_future VALUES LESS THAN MAXVALUE
    )
);

-- 在线统计表
CREATE TABLE IF NOT EXISTS online_statistics (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    online_players INT DEFAULT 0,
    peak_players INT DEFAULT 0,
    new_registrations INT DEFAULT 0,
    server_id VARCHAR(32),
    region VARCHAR(32),
    
    INDEX idx_timestamp (timestamp),
    INDEX idx_server (server_id, timestamp)
);

-- 性能监控表
CREATE TABLE IF NOT EXISTS performance_metrics (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    metric_name VARCHAR(64) NOT NULL,
    metric_value DOUBLE,
    server_id VARCHAR(32),
    timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    INDEX idx_metric_name (metric_name),
    INDEX idx_timestamp (timestamp),
    INDEX idx_server_metric (server_id, metric_name, timestamp)
);

-- =====================================
-- 8. 系统配置表
-- =====================================

-- 服务器配置表
CREATE TABLE IF NOT EXISTS server_config (
    config_key VARCHAR(128) PRIMARY KEY,
    config_value TEXT,
    config_type VARCHAR(32) DEFAULT 'string', -- string, int, float, bool, json
    description TEXT,
    is_runtime_modifiable BOOLEAN DEFAULT TRUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

-- 游戏常量表（物品、技能等配置）
CREATE TABLE IF NOT EXISTS game_constants (
    constant_type VARCHAR(64),
    constant_key VARCHAR(128),
    constant_value TEXT,
    version INT DEFAULT 1,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    
    PRIMARY KEY (constant_type, constant_key),
    INDEX idx_constant_type (constant_type),
    INDEX idx_version (version)
);

-- =====================================
-- 9. 初始化数据
-- =====================================

-- 插入默认服务器配置
INSERT INTO server_config (config_key, config_value, config_type, description) VALUES
('max_players_per_server', '1000', 'int', 'Maximum players per server'),
('exp_rate', '1.0', 'float', 'Experience gain multiplier'),
('drop_rate', '1.0', 'float', 'Item drop rate multiplier'),
('pvp_enabled', 'true', 'bool', 'PvP system enabled'),
('maintenance_mode', 'false', 'bool', 'Server maintenance mode'),
('welcome_message', 'Welcome to Shield Game Server!', 'string', 'New player welcome message');

-- 插入默认地图区域
INSERT INTO map_regions (name, min_x, max_x, min_y, max_y, min_z, max_z, region_type, level_requirement, description) VALUES
('Newbie Village', -100, 100, -100, 100, 0, 50, 'safe', 1, 'Starting area for new players'),
('Dark Forest', 100, 500, 100, 500, 0, 100, 'pvp', 10, 'Dangerous forest with valuable resources'),
('Dragon Lair', 1000, 1200, 1000, 1200, 100, 200, 'dungeon', 50, 'High-level dungeon with epic loot');

-- 插入示例任务模板
INSERT INTO quest_templates (name, description, quest_type, level_requirement, objectives, rewards) VALUES
('Kill 10 Rats', 'The village is overrun with rats. Help us by killing 10 of them.', 'kill', 1, 
 '{"kill": {"rat": 10}}', '{"experience": 100, "gold": 50}'),
('Collect 5 Herbs', 'The healer needs herbs for potions. Collect 5 healing herbs.', 'collect', 3,
 '{"collect": {"healing_herb": 5}}', '{"experience": 150, "gold": 75, "items": [{"id": 101, "name": "Health Potion", "quantity": 2}]}');

-- =====================================
-- 10. 索引优化建议
-- =====================================

-- 玩家相关复合索引
CREATE INDEX idx_players_level_exp ON players(level, experience);
CREATE INDEX idx_players_position ON players(position_x, position_y, position_z);
CREATE INDEX idx_players_active ON players(last_login) WHERE last_login > DATE_SUB(NOW(), INTERVAL 30 DAY);

-- 物品相关复合索引
CREATE INDEX idx_player_items_slot ON player_items(player_id, slot_type, slot_index);
CREATE INDEX idx_player_items_type ON player_items(player_id, item_id);

-- 拍卖行复合索引  
CREATE INDEX idx_auction_active ON auction_house(status, expires_at) WHERE status = 'active';
CREATE INDEX idx_auction_item_price ON auction_house(item_id, buyout_price) WHERE status = 'active';

-- 日志表复合索引
CREATE INDEX idx_game_events_player_type ON game_events(player_id, event_type, timestamp);
CREATE INDEX idx_game_events_recent ON game_events(timestamp, event_type) WHERE timestamp > DATE_SUB(NOW(), INTERVAL 7 DAY);

-- =====================================
-- 11. 视图定义
-- =====================================

-- 玩家完整信息视图
CREATE VIEW player_full_info AS
SELECT 
    p.*,
    g.name as guild_name,
    gm.rank as guild_rank,
    pvp.rating as pvp_rating,
    pvp.wins as pvp_wins,
    pvp.losses as pvp_losses
FROM players p
LEFT JOIN guild_members gm ON p.id = gm.player_id
LEFT JOIN guilds g ON gm.guild_id = g.id
LEFT JOIN pvp_rankings pvp ON p.id = pvp.player_id;

-- 在线玩家统计视图
CREATE VIEW online_players_stats AS
SELECT 
    COUNT(*) as total_online,
    AVG(level) as avg_level,
    MAX(level) as max_level,
    COUNT(CASE WHEN last_login > DATE_SUB(NOW(), INTERVAL 1 HOUR) THEN 1 END) as active_last_hour
FROM players 
WHERE last_login > DATE_SUB(NOW(), INTERVAL 10 MINUTE);

-- 服务器健康状态视图
CREATE VIEW server_health AS
SELECT 
    (SELECT COUNT(*) FROM players WHERE last_login > DATE_SUB(NOW(), INTERVAL 10 MINUTE)) as online_players,
    (SELECT COUNT(*) FROM auction_house WHERE status = 'active') as active_auctions,
    (SELECT COUNT(*) FROM game_events WHERE timestamp > DATE_SUB(NOW(), INTERVAL 1 HOUR)) as events_last_hour,
    (SELECT AVG(metric_value) FROM performance_metrics WHERE metric_name = 'cpu_usage' AND timestamp > DATE_SUB(NOW(), INTERVAL 5 MINUTE)) as avg_cpu_usage;