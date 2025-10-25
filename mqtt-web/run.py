#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
智能晾衣架控制面板 - 启动脚本
基于Flask + SocketIO + MQTT的物联网控制系统
"""

import os
import sys
from app import app, socketio, db, init_mqtt, User

if __name__ == '__main__':
    # 设置环境变量
    os.environ['FLASK_ENV'] = 'development'
    
    print("=" * 50)
    print("智能晾衣架控制面板")
    print("=" * 50)
    print("启动服务器...")
    print("访问地址: http://localhost:5000")
    print("按 Ctrl+C 停止服务器")
    print("=" * 50)
    
    try:
        # 创建数据库表
        with app.app_context():
            db.create_all()
            
            # 创建默认用户 - 与原项目保持一致
            users_to_create = [
                ('admin', 'admin123'),
                ('user', 'user123'),
                ('manager', 'manager456')
            ]
            
            for username, password in users_to_create:
                if not User.query.filter_by(username=username).first():
                    user = User(username=username, password=password)
                    db.session.add(user)
            
            db.session.commit()
        
        # 初始化MQTT
        init_mqtt()
        
        # 启动应用
        socketio.run(
            app, 
            host='0.0.0.0', 
            port=5000, 
            debug=True,
            use_reloader=True,
            log_output=True,
            allow_unsafe_werkzeug=True
        )
    except KeyboardInterrupt:
        print("\n服务器已停止")
        sys.exit(0)
    except Exception as e:
        print(f"启动失败: {e}")
        sys.exit(1)