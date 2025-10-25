from flask import Flask, render_template, jsonify, request, session, redirect, url_for
from flask_socketio import SocketIO, emit
from flask_sqlalchemy import SQLAlchemy
from datetime import datetime, timedelta
import json
import os
import threading
import time
import paho.mqtt.client as mqtt
import random
from functools import wraps

app = Flask(__name__)
app.config['SECRET_KEY'] = 'smart-clothesline-secret-key-2024'
app.config['SQLALCHEMY_DATABASE_URI'] = 'sqlite:///clothesline.db'
app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False

db = SQLAlchemy()
db.init_app(app)
socketio = SocketIO(app, cors_allowed_origins="*", async_mode='threading')

# MQTT配置 - 与原静态web完全一致
MQTT_BROKER = "broker.emqx.io"
MQTT_PORT = 1883
DEVICE_ID = "hust666"

# MQTT主题 - 与原项目保持一致
TOPIC_SENSORS_BASE = f"smart_clothesline/{DEVICE_ID}/sensors"
TOPIC_CONTROL = f"smart_clothesline/{DEVICE_ID}/control"
TOPIC_MODE_SET = f"smart_clothesline/{DEVICE_ID}/mode/set"  # 模式设置主题
TOPIC_STATUS = f"smart_clothesline/{DEVICE_ID}/status"
TOPIC_REQUEST = f"smart_clothesline/{DEVICE_ID}/request"

# 配置参数
DATA_COLLECTION_INTERVAL = 5  # 数据采集间隔（秒），默认5秒
FRONTEND_REQUEST_INTERVAL = 30  # 前端请求间隔（秒），默认30秒
HISTORY_DATA_INTERVAL = 10  # 历史数据显示间隔（秒），默认10秒
HISTORY_DATA_COUNT = 10  # 历史数据显示数量，默认10个数据点

# 全局变量
mqtt_client = None
current_sensor_data = {
    "temperature": 25.0,
    "humidity": 60.0,
    "light": 500
}
clothesline_status = "retract"
work_mode = "auto"  # 当前工作模式：auto或manual
last_data_save_time = 0  # 上次保存数据的时间戳
mode_switch_lock_time = 0  # 模式切换锁定时间戳
MODE_SWITCH_LOCK_DURATION = 3  # 模式切换锁定持续时间（秒）

# 登录验证配置 - 与原静态web完全一致
LOGIN_CREDENTIALS = {
    'admin': 'admin123',
    'user': 'user123',
    'manager': 'manager456'
}

# 数据库模型
class SensorData(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    sensor_type = db.Column(db.String(20), nullable=False)  # temperature, humidity, light
    value = db.Column(db.Float, nullable=False)
    timestamp = db.Column(db.DateTime, default=datetime.utcnow)
    
    def to_dict(self):
        return {
            'id': self.id,
            'sensor_type': self.sensor_type,
            'value': self.value,
            'timestamp': self.timestamp.strftime('%Y-%m-%d %H:%M:%S')
        }

class ClotheslineStatus(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    status = db.Column(db.String(20), nullable=False)
    timestamp = db.Column(db.DateTime, default=datetime.utcnow)
    
    def to_dict(self):
        return {
            'id': self.id,
            'status': self.status,
            'timestamp': self.timestamp.strftime('%Y-%m-%d %H:%M:%S')
        }

class User(db.Model):
    id = db.Column(db.Integer, primary_key=True)
    username = db.Column(db.String(80), unique=True, nullable=False)
    password = db.Column(db.String(120), nullable=False)
    last_login = db.Column(db.DateTime)
    
    def to_dict(self):
        return {
            'id': self.id,
            'username': self.username,
            'last_login': self.last_login.strftime('%Y-%m-%d %H:%M:%S') if self.last_login else None
        }

# 登录验证装饰器
def login_required(f):
    @wraps(f)
    def decorated_function(*args, **kwargs):
        if 'user' not in session:
            if request.is_json or request.path.startswith('/api/'):
                return jsonify({"success": False, "message": "请先登录", "require_login": True}), 401
            return redirect(url_for('login_page'))
        return f(*args, **kwargs)
    return decorated_function

# MQTT回调函数
def on_connect(client, userdata, flags, rc):
    print(f"✅ MQTT连接状态: {rc}")
    if rc == 0:
        # 订阅基础传感器主题（用于接收完整JSON数据）
        client.subscribe(TOPIC_SENSORS_BASE, qos=0)
        # 订阅所有传感器子主题（兼容性）
        client.subscribe(f"{TOPIC_SENSORS_BASE}/+", qos=0)
        client.subscribe(TOPIC_STATUS, qos=0)
        print(f"📡 已订阅MQTT主题:")
        print(f"   - {TOPIC_SENSORS_BASE}")
        print(f"   - {TOPIC_SENSORS_BASE}/+")
        print(f"   - {TOPIC_STATUS}")
        
        # MQTT重连后，向所有WebSocket客户端推送当前状态以确保同步
        socketio.emit('status_update', {'status': clothesline_status})
        socketio.emit('mqtt_status', {'connected': True})
        print(f"🔄 MQTT重连后同步状态: {clothesline_status}")
        print("📡 已通知前端MQTT连接成功")
    else:
        print(f"❌ MQTT连接失败: {rc}")

def on_message(client, userdata, msg):
    global current_sensor_data, clothesline_status, last_data_save_time
    
    try:
        topic = msg.topic
        payload_str = msg.payload.decode()
        
        print(f"📨 收到MQTT消息: {topic} -> {payload_str}")
        
        # 检查是否需要限制数据保存频率
        current_time = time.time()
        should_save_to_db = (current_time - last_data_save_time) >= DATA_COLLECTION_INTERVAL
        
        # 在Flask应用上下文中处理消息
        with app.app_context():
            # 处理传感器数据
            if topic.startswith(TOPIC_SENSORS_BASE):
                # 检查是否是完整的传感器数据JSON (发送到基础主题)
                if topic == TOPIC_SENSORS_BASE:
                    try:
                        # 尝试解析JSON格式的完整传感器数据
                        sensor_json = json.loads(payload_str)
                        
                        # 更新当前传感器数据
                        if 'temperature' in sensor_json:
                            current_sensor_data['temperature'] = float(sensor_json['temperature'])
                        if 'humidity' in sensor_json:
                            current_sensor_data['humidity'] = float(sensor_json['humidity'])
                        if 'light_intensity' in sensor_json:
                            current_sensor_data['light'] = int(sensor_json['light_intensity'])
                        
                        # 更新晾衣架状态（如果包含在传感器数据中）
                        if 'clothesline_status' in sensor_json:
                            raw_status = sensor_json['clothesline_status']
                            
                            # 简化状态映射：只保留extend和retract
                            if raw_status in ['extending', 'extended', 'extend']:
                                clothesline_status = 'extend'
                            elif raw_status in ['retracting', 'retracted', 'retract']:
                                clothesline_status = 'retract'
                            else:
                                clothesline_status = 'retract'  # 默认为收回状态
                            
                            # 只在满足时间间隔时保存状态到数据库
                            if should_save_to_db:
                                status_record = ClotheslineStatus(status=clothesline_status)
                                db.session.add(status_record)
                                
                                # 保持最多10条状态记录
                                count = ClotheslineStatus.query.count()
                                if count > 10:
                                    oldest = ClotheslineStatus.query.order_by(ClotheslineStatus.timestamp.asc()).first()
                                    if oldest:
                                        db.session.delete(oldest)
                            
                            # 推送状态更新（实时推送，不受频率限制）
                            socketio.emit('status_update', {'status': clothesline_status})
                            print(f"🔄 JSON晾衣架状态更新: {raw_status} -> {clothesline_status}")
                        
                        # 更新工作模式（如果包含在传感器数据中）
                        if 'work_mode' in sensor_json:
                            global work_mode, mode_switch_lock_time
                            received_mode = sensor_json['work_mode']
                            current_time = time.time()
                            
                            # 检查是否在模式切换锁定期间
                            if current_time - mode_switch_lock_time < MODE_SWITCH_LOCK_DURATION:
                                print(f"🔒 模式切换锁定中，忽略来自硬件的模式更新: {received_mode} (剩余锁定时间: {MODE_SWITCH_LOCK_DURATION - (current_time - mode_switch_lock_time):.1f}秒)")
                            else:
                                # 只在非锁定期间更新模式状态
                                if received_mode != work_mode:
                                    work_mode = received_mode
                                    print(f"🔄 工作模式同步更新: {work_mode}")
                                    # 推送模式更新到前端
                                    socketio.emit('sensor_update', {'work_mode': work_mode})
                        
                        # 只在满足时间间隔时保存传感器数据到数据库
                        if should_save_to_db:
                            for sensor_type, value in [
                                ('temperature', sensor_json.get('temperature')),
                                ('humidity', sensor_json.get('humidity')),
                                ('light', sensor_json.get('light_intensity'))
                            ]:
                                if value is not None:
                                    sensor_record = SensorData(
                                        sensor_type=sensor_type,
                                        value=float(value)
                                    )
                                    db.session.add(sensor_record)
                                    
                                    # 保持最多10条记录
                                    count = SensorData.query.filter_by(sensor_type=sensor_type).count()
                                    if count > 10:
                                        oldest = SensorData.query.filter_by(sensor_type=sensor_type).order_by(SensorData.timestamp.asc()).first()
                                        if oldest:
                                            db.session.delete(oldest)
                            
                            # 提交数据库更改并更新时间戳
                            db.session.commit()
                            last_data_save_time = current_time
                            print(f"💾 数据已保存到数据库 (间隔: {DATA_COLLECTION_INTERVAL}秒)")
                        else:
                            print(f"⏱️ 跳过数据库保存 (距离上次保存: {current_time - last_data_save_time:.1f}秒)")
                        
                        # 通过WebSocket推送完整传感器数据给前端（包含工作模式）
                        sensor_data_with_mode = current_sensor_data.copy()
                        sensor_data_with_mode['work_mode'] = work_mode
                        sensor_data_with_mode['clothesline_status'] = clothesline_status
                        socketio.emit('sensor_data', sensor_data_with_mode)
                        print(f"🔄 传感器数据更新: {sensor_data_with_mode}")
                        
                    except json.JSONDecodeError:
                        # 如果不是JSON格式，尝试解析ESP32发送的字符串格式
                        # 格式示例: "T=23.0°C H=57.0% L=563 晾衣架状态=extending"
                        print(f"📝 尝试解析ESP32字符串格式: {payload_str}")
                        
                        # 解析温度
                        import re
                        temp_match = re.search(r'T=([0-9.]+)', payload_str)
                        if temp_match:
                            current_sensor_data['temperature'] = float(temp_match.group(1))
                        
                        # 解析湿度
                        humidity_match = re.search(r'H=([0-9.]+)', payload_str)
                        if humidity_match:
                            current_sensor_data['humidity'] = float(humidity_match.group(1))
                        
                        # 解析光照强度
                        light_match = re.search(r'L=([0-9]+)', payload_str)
                        if light_match:
                            current_sensor_data['light'] = int(light_match.group(1))
                        
                        # 解析晾衣架状态 - 支持多种格式
                        status_match = re.search(r'晾衣架状态=(\w+)', payload_str)
                        if not status_match:
                            # 尝试其他可能的格式
                            status_match = re.search(r'status=(\w+)', payload_str)
                        if not status_match:
                            status_match = re.search(r'state=(\w+)', payload_str)
                            
                        if status_match:
                            raw_status = status_match.group(1)
                            
                            # 简化状态映射：只保留extend和retract
                            if raw_status in ['extending', 'extended', 'extend']:
                                clothesline_status = 'extend'
                            elif raw_status in ['retracting', 'retracted', 'retract']:
                                clothesline_status = 'retract'
                            else:
                                clothesline_status = 'retract'  # 默认为收回状态
                            
                            # 只在满足时间间隔时保存状态到数据库
                            if should_save_to_db:
                                status_record = ClotheslineStatus(status=clothesline_status)
                                db.session.add(status_record)
                                
                                # 保持最多10条状态记录
                                count = ClotheslineStatus.query.count()
                                if count > 10:
                                    oldest = ClotheslineStatus.query.order_by(ClotheslineStatus.timestamp.asc()).first()
                                    if oldest:
                                        db.session.delete(oldest)
                            
                            # 推送状态更新（实时推送，不受频率限制）
                            socketio.emit('status_update', {'status': clothesline_status})
                            print(f"🔄 晾衣架状态更新: {raw_status} -> {clothesline_status}")
                        
                        # 只在满足时间间隔时保存传感器数据到数据库
                        if should_save_to_db:
                            # 保存解析到的传感器数据
                            if temp_match:
                                sensor_record = SensorData(sensor_type='temperature', value=float(temp_match.group(1)))
                                db.session.add(sensor_record)
                                # 保持最多10条记录
                                count = SensorData.query.filter_by(sensor_type='temperature').count()
                                if count > 10:
                                    oldest = SensorData.query.filter_by(sensor_type='temperature').order_by(SensorData.timestamp.asc()).first()
                                    if oldest:
                                        db.session.delete(oldest)
                            
                            if humidity_match:
                                sensor_record = SensorData(sensor_type='humidity', value=float(humidity_match.group(1)))
                                db.session.add(sensor_record)
                                # 保持最多10条记录
                                count = SensorData.query.filter_by(sensor_type='humidity').count()
                                if count > 10:
                                    oldest = SensorData.query.filter_by(sensor_type='humidity').order_by(SensorData.timestamp.asc()).first()
                                    if oldest:
                                        db.session.delete(oldest)
                            
                            if light_match:
                                sensor_record = SensorData(sensor_type='light', value=float(light_match.group(1)))
                                db.session.add(sensor_record)
                                # 保持最多10条记录
                                count = SensorData.query.filter_by(sensor_type='light').count()
                                if count > 10:
                                    oldest = SensorData.query.filter_by(sensor_type='light').order_by(SensorData.timestamp.asc()).first()
                                    if oldest:
                                        db.session.delete(oldest)
                            
                            # 提交数据库更改并更新时间戳
                            db.session.commit()
                            last_data_save_time = current_time
                            print(f"💾 ESP32字符串数据已保存到数据库 (间隔: {DATA_COLLECTION_INTERVAL}秒)")
                        else:
                            print(f"⏱️ 跳过ESP32字符串数据保存 (距离上次保存: {current_time - last_data_save_time:.1f}秒)")
                        
                        # 通过WebSocket推送完整传感器数据给前端
                        socketio.emit('sensor_data', current_sensor_data)
                        print(f"🔄 ESP32传感器数据更新: {current_sensor_data}")
                        
                else:
                    # 处理单个传感器数据 (兼容性)
                    sensor_type = topic.split('/')[-1]  # 获取传感器类型 (temperature/humidity/light/clothesline_status)
                    
                    # 处理晾衣架状态主题
                    if sensor_type == 'clothesline_status':
                        raw_status = payload_str
                        
                        # 简化状态映射：只保留extend和retract
                        if raw_status in ['extending', 'extended', 'extend']:
                            clothesline_status = 'extend'
                        elif raw_status in ['retracting', 'retracted', 'retract']:
                            clothesline_status = 'retract'
                        else:
                            clothesline_status = 'retract'  # 默认为收回状态
                        
                        # 只在满足时间间隔时保存状态到数据库
                        if should_save_to_db:
                            status_record = ClotheslineStatus(status=clothesline_status)
                            db.session.add(status_record)
                            
                            # 保持最多10条状态记录
                            count = ClotheslineStatus.query.count()
                            if count > 10:
                                oldest = ClotheslineStatus.query.order_by(ClotheslineStatus.timestamp.asc()).first()
                                if oldest:
                                    db.session.delete(oldest)
                            
                            db.session.commit()
                            last_data_save_time = current_time
                            print(f"💾 晾衣架状态已保存: {clothesline_status} (间隔: {DATA_COLLECTION_INTERVAL}秒)")
                        else:
                            print(f"⏱️ 跳过晾衣架状态保存: {clothesline_status} (距离上次保存: {current_time - last_data_save_time:.1f}秒)")
                        
                        # 通过WebSocket推送状态更新给前端（实时推送，不受频率限制）
                        socketio.emit('status_update', {'status': clothesline_status})
                        print(f"🔄 晾衣架状态更新: {raw_status} -> {clothesline_status}")
                        
                    else:
                        # 处理传感器数据主题
                        value = float(payload_str)
                        
                        # 更新当前传感器数据
                        if sensor_type == 'temperature':
                            current_sensor_data['temperature'] = value
                        elif sensor_type == 'humidity':
                            current_sensor_data['humidity'] = value
                        elif sensor_type == 'light':
                            current_sensor_data['light'] = int(value)
                        
                        # 只在满足时间间隔时保存单个传感器数据到数据库
                        if should_save_to_db:
                            sensor_record = SensorData(
                                sensor_type=sensor_type,
                                value=value
                            )
                            db.session.add(sensor_record)
                            
                            # 保持最多10条记录
                            count = SensorData.query.filter_by(sensor_type=sensor_type).count()
                            if count > 10:
                                oldest = SensorData.query.filter_by(sensor_type=sensor_type).order_by(SensorData.timestamp.asc()).first()
                                if oldest:
                                    db.session.delete(oldest)
                            
                            db.session.commit()
                            last_data_save_time = current_time
                            print(f"💾 单个传感器数据已保存: {sensor_type}={value} (间隔: {DATA_COLLECTION_INTERVAL}秒)")
                        else:
                            print(f"⏱️ 跳过单个传感器数据保存: {sensor_type}={value} (距离上次保存: {current_time - last_data_save_time:.1f}秒)")
                        
                        # 通过WebSocket推送传感器数据给前端（实时推送，不受频率限制）
                        socketio.emit('sensor_data', {sensor_type: value})
                
            elif topic == TOPIC_STATUS:
                # 处理设备状态数据 - 只处理设备连接状态，不处理晾衣架状态
                try:
                    payload = json.loads(payload_str)
                    device_status = payload.get('status', payload_str)
                    status_type = payload.get('type', 'unknown')
                    print(f"📡 设备状态更新: {device_status} (类型: {status_type})")
                except:
                    device_status = payload_str
                    print(f"📡 设备状态更新: {device_status}")
                
                # 注意：设备状态（online/offline）与晾衣架状态（extend/retract）是不同的概念
                # 晾衣架状态只通过 sensors/clothesline_status 主题处理
            
    except Exception as e:
        print(f"❌ 处理MQTT消息错误: {e}")

def on_disconnect(client, userdata, rc):
    print(f"🔌 MQTT连接断开: {rc}")
    # 通知前端MQTT连接状态变化
    socketio.emit('mqtt_status', {'connected': False})
    print("📡 已通知前端MQTT断开")

# 从数据库恢复最新状态
def restore_latest_status():
    """从数据库恢复最新的晾衣架状态"""
    global clothesline_status
    try:
        with app.app_context():
            latest_status = ClotheslineStatus.query.order_by(ClotheslineStatus.timestamp.desc()).first()
            if latest_status:
                clothesline_status = latest_status.status
                print(f"🔄 从数据库恢复状态: {clothesline_status}")
            else:
                print(f"📝 使用默认状态: {clothesline_status}")
    except Exception as e:
        print(f"❌ 恢复状态失败: {e}")

# 初始化MQTT客户端
def init_mqtt():
    global mqtt_client
    mqtt_client = mqtt.Client()
    mqtt_client.on_connect = on_connect
    mqtt_client.on_message = on_message
    mqtt_client.on_disconnect = on_disconnect
    
    try:
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
        mqtt_client.loop_start()
        print("🚀 MQTT客户端已启动")
    except Exception as e:
        print(f"❌ MQTT连接失败: {e}")

# 生成示例数据 - 与原静态web一致
def generate_sample_data():
    """生成示例传感器数据用于演示"""
    sample_data = [
        {'temp': 22.5, 'hum': 60.2, 'light': 320},
        {'temp': 23.1, 'hum': 58.7, 'light': 380},
        {'temp': 24.3, 'hum': 62.1, 'light': 420},
        {'temp': 25.8, 'hum': 65.4, 'light': 450},
        {'temp': 26.2, 'hum': 63.9, 'light': 480},
        {'temp': 25.5, 'hum': 61.8, 'light': 440},
        {'temp': 24.9, 'hum': 59.3, 'light': 410},
        {'temp': 23.7, 'hum': 57.6, 'light': 390}
    ]
    
    with app.app_context():
        # 清除现有数据
        SensorData.query.delete()
        
        # 添加示例数据
        for i, data in enumerate(sample_data):
            timestamp = datetime.utcnow() - timedelta(minutes=(len(sample_data) - i) * 5)
            
            # 添加温度数据
            temp_record = SensorData(sensor_type='temperature', value=data['temp'], timestamp=timestamp)
            db.session.add(temp_record)
            
            # 添加湿度数据
            hum_record = SensorData(sensor_type='humidity', value=data['hum'], timestamp=timestamp)
            db.session.add(hum_record)
            
            # 添加光照数据
            light_record = SensorData(sensor_type='light', value=data['light'], timestamp=timestamp)
            db.session.add(light_record)
        
        db.session.commit()
        print("📊 已生成示例传感器数据")

# 路由
@app.route('/')
@login_required
def index():
    return render_template('index.html')

@app.route('/login')
def login_page():
    # 如果已经登录，重定向到主页
    if 'user' in session:
        return redirect(url_for('index'))
    return render_template('index.html')

@app.route('/api/login', methods=['POST'])
def login():
    data = request.get_json()
    username = data.get('username', '').strip()
    password = data.get('password', '').strip()
    
    if not username or not password:
        return jsonify({"success": False, "message": "用户名和密码不能为空"})
    
    if username in LOGIN_CREDENTIALS and LOGIN_CREDENTIALS[username] == password:
        # 更新用户最后登录时间
        with app.app_context():
            user = User.query.filter_by(username=username).first()
            if user:
                user.last_login = datetime.utcnow()
                db.session.commit()
        
        session['user'] = username
        session['login_time'] = datetime.utcnow().isoformat()
        
        return jsonify({"success": True, "message": "登录成功", "user": username})
    else:
        return jsonify({"success": False, "message": "用户名或密码错误"})

@app.route('/api/logout', methods=['POST'])
def logout():
    session.clear()
    return jsonify({"success": True, "message": "已退出登录"})

@app.route('/api/check-login', methods=['GET'])
def check_login():
    """检查登录状态"""
    if 'user' in session:
        return jsonify({
            "success": True, 
            "logged_in": True, 
            "user": session['user'],
            "login_time": session.get('login_time')
        })
    else:
        return jsonify({"success": True, "logged_in": False})

@app.route('/api/sensor-data')
@login_required
def get_sensor_data():
    """获取当前传感器数据"""
    # 创建包含晾衣架状态和工作模式的完整数据
    sensor_data_with_status = current_sensor_data.copy()
    sensor_data_with_status['clothesline_status'] = clothesline_status
    sensor_data_with_status['work_mode'] = work_mode
    
    return jsonify({
        "success": True,
        "data": sensor_data_with_status,
        "timestamp": datetime.now().isoformat()
    })

@app.route('/api/clothesline-status')
@login_required
def get_clothesline_status():
    """获取晾衣架状态"""
    return jsonify({
        "success": True,
        "status": clothesline_status,
        "timestamp": datetime.now().isoformat()
    })

@app.route('/api/config')
@login_required
def get_config():
    """获取系统配置参数"""
    return jsonify({
        "success": True,
        "config": {
            "data_collection_interval": DATA_COLLECTION_INTERVAL,
            "frontend_request_interval": FRONTEND_REQUEST_INTERVAL,
            "history_data_interval": HISTORY_DATA_INTERVAL,
            "history_data_count": HISTORY_DATA_COUNT
        }
    })

@app.route('/api/control', methods=['POST'])
@login_required
def control_clothesline():
    """控制晾衣架伸缩和模式切换"""
    global work_mode, mode_switch_lock_time
    data = request.get_json()
    action = data.get('action', '').strip()
    
    # 支持中英文指令 - 与原静态web一致
    valid_actions = ['extend', 'retract', '伸出', '收回', 'mode_auto', 'mode_manual', '自动模式', '手动模式']
    
    if action in valid_actions:
        # 检查自动模式下的手动操作
        manual_actions = ['extend', 'retract', '伸出', '收回']
        if action in manual_actions and work_mode == 'auto':
            return jsonify({"success": False, "message": "自动模式下无法进行手动操作，请先切换至手动模式"})
        
        # 统一转换为英文指令发送给硬件
        if action in ['伸出', 'extend']:
            mqtt_action = 'extend'
            mqtt_topic = TOPIC_CONTROL
        elif action in ['收回', 'retract']:
            mqtt_action = 'retract'
            mqtt_topic = TOPIC_CONTROL
        elif action in ['自动模式', 'mode_auto']:
            mqtt_action = 'auto'  # 发送简化的模式指令
            mqtt_topic = TOPIC_MODE_SET  # 使用专门的模式设置主题
            work_mode = 'auto'
            mode_switch_lock_time = time.time()  # 激活模式切换锁定
        elif action in ['手动模式', 'mode_manual']:
            mqtt_action = 'manual'  # 发送简化的模式指令
            mqtt_topic = TOPIC_MODE_SET  # 使用专门的模式设置主题
            work_mode = 'manual'
            mode_switch_lock_time = time.time()  # 激活模式切换锁定
        else:
            # 处理直接的英文指令
            if action == 'mode_auto':
                mqtt_action = 'auto'
                mqtt_topic = TOPIC_MODE_SET
                work_mode = 'auto'
                mode_switch_lock_time = time.time()  # 激活模式切换锁定
            elif action == 'mode_manual':
                mqtt_action = 'manual'
                mqtt_topic = TOPIC_MODE_SET
                work_mode = 'manual'
                mode_switch_lock_time = time.time()  # 激活模式切换锁定
            else:
                mqtt_action = action
                mqtt_topic = TOPIC_CONTROL
        
        if mqtt_client and mqtt_client.is_connected():
            mqtt_client.publish(mqtt_topic, mqtt_action, qos=1)
            print(f"🎮 发送指令到 {mqtt_topic}: {mqtt_action}")
            
            # 如果是模式切换，推送模式更新
            if mqtt_topic == TOPIC_MODE_SET:
                socketio.emit('sensor_update', {'work_mode': work_mode})
                print(f"🔄 模式切换: {work_mode}")
            
            return jsonify({"success": True, "message": f"已发送{action}指令"})
        else:
            return jsonify({"success": False, "message": "MQTT未连接，无法发送指令"})
    else:
        return jsonify({"success": False, "message": "无效的控制指令"})

@app.route('/api/history/<sensor_type>')
@login_required
def get_sensor_history(sensor_type):
    """获取指定传感器的历史数据 - 智能筛选确保足够数据"""
    if sensor_type not in ['temperature', 'humidity', 'light']:
        return jsonify({"success": False, "message": "无效的传感器类型"})
    
    # 获取所有记录，按时间倒序
    all_records = SensorData.query.filter_by(sensor_type=sensor_type)\
                                 .order_by(SensorData.timestamp.desc())\
                                 .all()
    
    if not all_records:
        return jsonify({
            "success": True,
            "type": sensor_type,
            "data": [],
            "interval_seconds": HISTORY_DATA_INTERVAL,
            "total_points": 0
        })
    
    # 智能筛选逻辑：优先按间隔筛选，如果数据不够则降级处理
    history = []
    
    # 方法1：尝试按配置的时间间隔筛选
    last_timestamp = None
    for record in all_records:
        if (last_timestamp is None or 
            abs((last_timestamp - record.timestamp).total_seconds()) >= HISTORY_DATA_INTERVAL):
            
            history.append({
                "value": record.value,
                "timestamp": record.timestamp.strftime("%Y-%m-%d %H:%M:%S")
            })
            
            last_timestamp = record.timestamp
            
            if len(history) >= HISTORY_DATA_COUNT:
                break
    
    # 方法2：如果按间隔筛选得到的数据不够，则使用更小的间隔或直接取最近的数据
    if len(history) < HISTORY_DATA_COUNT:
        # 计算实际可用的时间间隔
        if len(all_records) >= HISTORY_DATA_COUNT:
            # 取最近的HISTORY_DATA_COUNT条记录
            recent_records = all_records[:HISTORY_DATA_COUNT]
            history = []
            for record in recent_records:
                history.append({
                    "value": record.value,
                    "timestamp": record.timestamp.strftime("%Y-%m-%d %H:%M:%S")
                })
        else:
            # 如果总数据都不够，就返回所有数据
            history = []
            for record in all_records:
                history.append({
                    "value": record.value,
                    "timestamp": record.timestamp.strftime("%Y-%m-%d %H:%M:%S")
                })
    
    # 按时间正序排列（从旧到新）
    history.reverse()
    
    # 计算实际使用的时间间隔
    actual_interval = HISTORY_DATA_INTERVAL
    if len(history) > 1:
        # 计算平均时间间隔
        from datetime import datetime
        timestamps = [datetime.strptime(h["timestamp"], "%Y-%m-%d %H:%M:%S") for h in history]
        if len(timestamps) > 1:
            total_duration = (timestamps[-1] - timestamps[0]).total_seconds()
            actual_interval = total_duration / (len(timestamps) - 1) if len(timestamps) > 1 else HISTORY_DATA_INTERVAL
    
    return jsonify({
        "success": True,
        "type": sensor_type,
        "data": history,
        "interval_seconds": int(actual_interval),
        "total_points": len(history)
    })

@app.route('/api/history')
@login_required
def get_all_history():
    """获取所有传感器的历史数据 - 智能筛选确保足够数据"""
    result = {}
    
    for sensor_type in ['temperature', 'humidity', 'light']:
        # 获取所有记录，按时间倒序
        all_records = SensorData.query.filter_by(sensor_type=sensor_type)\
                                     .order_by(SensorData.timestamp.desc())\
                                     .all()
        
        history = []
        if all_records:
            # 智能筛选逻辑：优先按间隔筛选，如果数据不够则降级处理
            
            # 方法1：尝试按配置的时间间隔筛选
            last_timestamp = None
            for record in all_records:
                if (last_timestamp is None or 
                    abs((last_timestamp - record.timestamp).total_seconds()) >= HISTORY_DATA_INTERVAL):
                    
                    history.append({
                        "value": record.value,
                        "timestamp": record.timestamp.strftime("%Y-%m-%d %H:%M:%S")
                    })
                    
                    last_timestamp = record.timestamp
                    
                    if len(history) >= HISTORY_DATA_COUNT:
                        break
            
            # 方法2：如果按间隔筛选得到的数据不够，则使用更小的间隔或直接取最近的数据
            if len(history) < HISTORY_DATA_COUNT:
                # 计算实际可用的时间间隔
                if len(all_records) >= HISTORY_DATA_COUNT:
                    # 取最近的HISTORY_DATA_COUNT条记录
                    recent_records = all_records[:HISTORY_DATA_COUNT]
                    history = []
                    for record in recent_records:
                        history.append({
                            "value": record.value,
                            "timestamp": record.timestamp.strftime("%Y-%m-%d %H:%M:%S")
                        })
                else:
                    # 如果总数据都不够，就返回所有数据
                    history = []
                    for record in all_records:
                        history.append({
                            "value": record.value,
                            "timestamp": record.timestamp.strftime("%Y-%m-%d %H:%M:%S")
                        })
            
            # 按时间正序排列（从旧到新）
            history.reverse()
        
        result[sensor_type] = history
    
    return jsonify({
        "success": True,
        "data": result,
        "config": {
            "interval_seconds": HISTORY_DATA_INTERVAL,
            "max_points": HISTORY_DATA_COUNT
        }
    })

@app.route('/api/clear-history', methods=['POST'])
@login_required
def clear_history():
    """清空历史数据"""
    data = request.get_json() or {}
    sensor_type = data.get('type')
    
    try:
        if sensor_type and sensor_type in ['temperature', 'humidity', 'light']:
            # 清空指定传感器的历史数据
            SensorData.query.filter_by(sensor_type=sensor_type).delete()
            message = f"{sensor_type}历史数据已清除"
        else:
            # 清空所有历史数据
            SensorData.query.delete()
            ClotheslineStatus.query.delete()
            message = "所有历史数据已清除"
        
        db.session.commit()
        return jsonify({"success": True, "message": message})
    except Exception as e:
        db.session.rollback()
        return jsonify({"success": False, "message": f"清除失败: {str(e)}"})

# @app.route('/api/request-data', methods=['POST'])
# @login_required
# def request_current_data():
#     """请求硬件发送最新数据"""
#     if mqtt_client and mqtt_client.is_connected():
#         request_msg = {
#             "action": "request_data",
#             "timestamp": datetime.now().isoformat(),
#             "device_id": DEVICE_ID
#         }
#         mqtt_client.publish(TOPIC_REQUEST, json.dumps(request_msg), qos=1)
#         return jsonify({"success": True, "message": "已请求最新数据"})
#     else:
#         return jsonify({"success": False, "message": "MQTT未连接"})

# WebSocket事件 - 与原静态web保持一致
@socketio.on('connect')
def handle_connect():
    print('👤 客户端已连接')
    # 发送当前数据给新连接的客户端（包含工作模式）
    sensor_data_with_mode = current_sensor_data.copy()
    sensor_data_with_mode['work_mode'] = work_mode
    sensor_data_with_mode['clothesline_status'] = clothesline_status
    emit('sensor_data', sensor_data_with_mode)
    emit('status_update', {"status": clothesline_status})

@socketio.on('disconnect')
def handle_disconnect():
    print('👤 客户端已断开连接')

# @socketio.on('request_data')
# def handle_request_data():
#     """WebSocket请求数据"""
#     if mqtt_client and mqtt_client.is_connected():
#         request_msg = {
#             "action": "request_data",
#             "timestamp": datetime.now().isoformat(),
#             "device_id": DEVICE_ID
#         }
#         mqtt_client.publish(TOPIC_REQUEST, json.dumps(request_msg), qos=1)
#         emit('message', {"type": "info", "content": "已请求最新数据"})
#     else:
#         emit('message', {"type": "error", "content": "MQTT未连接"})

if __name__ == '__main__':
    print("🚀 启动智能晾衣架控制系统...")
    
    with app.app_context():
        # 创建数据库表
        db.create_all()
        print("📊 数据库表已创建")
        
        # 添加默认用户
        users_added = 0
        for username, password in LOGIN_CREDENTIALS.items():
            if not User.query.filter_by(username=username).first():
                user = User(username=username, password=password)
                db.session.add(user)
                users_added += 1
        
        if users_added > 0:
            db.session.commit()
            print(f"👤 已添加 {users_added} 个默认用户")
        
        # 生成示例数据（如果数据库为空）
        if SensorData.query.count() == 0:
            generate_sample_data()
        
        # 从数据库恢复最新的晾衣架状态
        restore_latest_status()
    
    # 初始化MQTT
    init_mqtt()
    
    print("🌐 启动Web服务器...")
    print("📱 访问地址: http://localhost:5000")
    print("👤 默认账户: admin/admin123, user/user123, manager/manager456")
    print(f"⏱️ 数据采集间隔: {DATA_COLLECTION_INTERVAL}秒")
    print(f"🔄 前端请求间隔: {FRONTEND_REQUEST_INTERVAL}秒")
    print(f"📊 历史数据间隔: {HISTORY_DATA_INTERVAL}秒 (显示{HISTORY_DATA_COUNT}个数据点)")
    print("=" * 50)
    
    # 启动应用
    try:
        socketio.run(app, host='0.0.0.0', port=5000, debug=True, allow_unsafe_werkzeug=True)
    except KeyboardInterrupt:
        print("\n🛑 服务器已停止")
    except Exception as e:
        print(f"❌ 服务器启动失败: {e}")