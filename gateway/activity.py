"""In-memory hosting activity; camera and automatic prompts remain disabled."""

from dataclasses import dataclass, field


SCENES = {
    "social": "同桌交流：帮助参与者找到共同兴趣，不强制轮流发言。",
    "date": "用户主动选择的约会主题：轻松了解兴趣，不假定关系，不追问私密信息。",
    "meeting": "用户确认的小会议：围绕议题简短提示，不做考勤、身份判断或人员评价。",
}
THEMES = {
    "travel": ("旅行", "如果有三天假期，你想去哪里放松？"),
    "film": ("电影", "最近有没有一部让你想推荐的电影？"),
    "reading": ("阅读", "有没有一本书，让你记住了一句话？"),
    "general": ("自由话题", "今天大家最想一起聊聊什么？"),
}
MAX_HISTORY_TURNS = 6
MAX_MESSAGE_CHARS = 2000


@dataclass
class Activity:
    state: str = "none"
    scene: str = "social"
    theme: str = "travel"
    history: list[dict[str, str]] = field(default_factory=list)

    @classmethod
    def from_request(cls, request: dict):
        if request.get("consent") is not True:
            raise ValueError("consent_required")
        scene, theme = request.get("scene", "social"), request.get("theme", "travel")
        if not isinstance(scene, str) or scene not in SCENES:
            raise ValueError("invalid_scene")
        if not isinstance(theme, str) or theme not in THEMES:
            raise ValueError("invalid_theme")
        if request.get("camera_enabled", False) is not False or request.get("hints_enabled", False) is not False:
            raise ValueError("feature_not_ready")
        return cls(state="active", scene=scene, theme=theme)

    def snapshot(self):
        return {"state": self.state, "scene": self.scene, "theme": self.theme,
                "theme_label": THEMES[self.theme][0], "opener": THEMES[self.theme][1],
                "history_turns": len(self.history) // 2,
                "camera_enabled": False, "hints_enabled": False}

    def context(self):
        return f"当前活动场景：{SCENES[self.scene]} 当前主题：{THEMES[self.theme][0]}。尊重用户换题和跳过。"

    def remember(self, user: str, assistant: str):
        self.history.extend([
            {"role": "user", "content": user[:MAX_MESSAGE_CHARS]},
            {"role": "assistant", "content": assistant[:MAX_MESSAGE_CHARS]},
        ])
        del self.history[:-MAX_HISTORY_TURNS * 2]

    def end(self):
        self.history.clear()
        self.state = "ended"
        self.scene = "social"
        self.theme = "travel"
