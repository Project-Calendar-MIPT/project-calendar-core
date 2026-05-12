"""
Integration tests for Project Calendar API
Tests cover authentication, task management, and calendar functionality
"""

import os
import time
from pathlib import Path
from typing import Dict, Optional

import pytest
import requests
import yaml

# Configuration
BASE_URL = os.getenv("TEST_BASE_URL", "http://localhost:8080")
API_PREFIX = "/api"


class APIClient:
    """Helper class for API requests"""

    def __init__(self, base_url: str = BASE_URL):
        self.base_url = base_url
        self.token: Optional[str] = None
        self.user_id: Optional[str] = None

    def set_token(self, token: str, user_id: str):
        """Set authentication token"""
        self.token = token
        self.user_id = user_id

    def headers(self) -> Dict[str, str]:
        """Get headers with authentication"""
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        return headers

    def post(self, endpoint: str, data: dict, auth: bool = False):
        """POST request"""
        url = f"{self.base_url}{API_PREFIX}{endpoint}"
        headers = self.headers() if auth else {"Content-Type": "application/json"}
        response = requests.post(url, json=data, headers=headers)
        return response

    def get(self, endpoint: str, params: dict = None, auth: bool = True):
        """GET request"""
        url = f"{self.base_url}{API_PREFIX}{endpoint}"
        response = requests.get(
            url, params=params, headers=self.headers() if auth else {}
        )
        return response

    def put(self, endpoint: str, data: dict, auth: bool = True):
        """PUT request"""
        url = f"{self.base_url}{API_PREFIX}{endpoint}"
        response = requests.put(url, json=data, headers=self.headers() if auth else {})
        return response

    def patch(self, endpoint: str, data: dict, auth: bool = True):
        """PATCH request"""
        url = f"{self.base_url}{API_PREFIX}{endpoint}"
        response = requests.patch(
            url, json=data, headers=self.headers() if auth else {}
        )
        return response

    def delete(self, endpoint: str, auth: bool = True, params: dict = None):
        """DELETE request"""
        url = f"{self.base_url}{API_PREFIX}{endpoint}"
        response = requests.delete(
            url, params=params, headers=self.headers() if auth else {}
        )
        return response


@pytest.fixture(scope="session")
def wait_for_backend():
    """Wait for backend to be ready"""
    max_retries = 30
    retry_delay = 2

    for i in range(max_retries):
        try:
            response = requests.get(f"{BASE_URL}/", timeout=5)
            if response.status_code in [200, 404]:  # Either works, just need connection
                print(f"Backend is ready after {i + 1} attempts")
                return True
        except requests.exceptions.RequestException:
            if i < max_retries - 1:
                time.sleep(retry_delay)
            else:
                raise

    raise Exception("Backend did not start in time")


@pytest.fixture
def client(wait_for_backend):
    """Create API client"""
    return APIClient()


@pytest.fixture
def registered_user(client):
    """Register a test user and return authenticated client"""
    import uuid

    email = f"test_{uuid.uuid4().hex[:8]}@example.com"

    user_data = {
        "email": email,
        "password": "TestPassword123!",
        "display_name": "Test User",
        "name": "Test",
        "surname": "User",
        "work_schedule": [
            {"weekday": 0, "start_time": "09:00:00", "end_time": "18:00:00"},
            {"weekday": 1, "start_time": "09:00:00", "end_time": "18:00:00"},
            {"weekday": 2, "start_time": "09:00:00", "end_time": "18:00:00"},
            {"weekday": 3, "start_time": "09:00:00", "end_time": "18:00:00"},
            {"weekday": 4, "start_time": "09:00:00", "end_time": "18:00:00"},
        ],
    }

    response = client.post("/auth/register", user_data)
    assert response.status_code == 201, f"Registration failed: {response.text}"

    data = response.json()
    assert "token" in data
    assert "user" in data

    client.set_token(data["token"], data["user"]["id"])

    yield client

    # Teardown: удаляем все задачи, созданные тестовым пользователем.
    # Удаление корневых задач каскадно удаляет подзадачи.
    try:
        tasks_resp = client.get("/tasks", auth=True)
        if tasks_resp.status_code == 200:
            tasks = tasks_resp.json()
            if isinstance(tasks, list):
                roots = [t for t in tasks if not t.get("parent_task_id")]
                for t in roots:
                    client.delete(f"/tasks/{t['id']}", auth=True)
    except Exception:
        pass


class TestAuthentication:
    """Test authentication endpoints"""

    def test_register_user_success(self, client):
        """Test successful user registration"""
        import uuid

        email = f"newuser_{uuid.uuid4().hex[:8]}@example.com"

        user_data = {
            "email": email,
            "password": "SecurePass123!",
            "display_name": "New User",
            "work_schedule": [
                {"weekday": 0, "start_time": "09:00:00", "end_time": "17:00:00"}
            ],
        }

        response = client.post("/auth/register", user_data)
        assert response.status_code == 201

        data = response.json()
        assert data["success"] == True
        assert "token" in data
        assert data["user"]["email"] == email
        assert data["user"]["display_name"] == "New User"

    def test_register_duplicate_email(self, registered_user):
        """Test registration with duplicate email"""
        # Get the email from first user
        response = registered_user.get("/auth/me", auth=True)
        email = response.json()["email"]

        # Try to register with same email
        user_data = {
            "email": email,
            "password": "AnotherPass123!",
            "display_name": "Duplicate User",
            "work_schedule": [
                {"weekday": 0, "start_time": "09:00:00", "end_time": "17:00:00"}
            ],
        }

        response = registered_user.post("/auth/register", user_data)
        assert response.status_code == 409  # Conflict

    def test_register_sunday_weekday_iso(self, client):
        """Test registration with Sunday as weekday=7 (ISO) is accepted and stored as 0"""
        import uuid

        email = f"sunday_{uuid.uuid4().hex[:8]}@example.com"

        user_data = {
            "email": email,
            "password": "SecurePass123!",
            "display_name": "Sunday User",
            "work_schedule": [
                {"weekday": 7, "start_time": "10:00:00", "end_time": "14:00:00"}
            ],
        }

        response = client.post("/auth/register", user_data)
        assert response.status_code == 201, response.text

    def test_register_invalid_password(self, client):
        """Test registration with empty password is rejected"""
        import uuid

        user_data = {
            "email": f"test_{uuid.uuid4().hex[:8]}@example.com",
            "password": "",
            "display_name": "Test",
            "work_schedule": [
                {"weekday": 0, "start_time": "09:00:00", "end_time": "17:00:00"}
            ],
        }

        response = client.post("/auth/register", user_data)
        assert response.status_code == 400

    def test_login_success(self, client):
        """Test successful login"""
        import uuid

        email = f"login_{uuid.uuid4().hex[:8]}@example.com"
        password = "LoginPass123!"

        # Register user first
        user_data = {
            "email": email,
            "password": password,
            "display_name": "Login User",
            "work_schedule": [
                {"weekday": 0, "start_time": "09:00:00", "end_time": "17:00:00"}
            ],
        }
        client.post("/auth/register", user_data)

        # Now login
        login_data = {"email": email, "password": password}
        response = client.post("/auth/login", login_data)

        assert response.status_code == 200
        data = response.json()
        assert "token" in data
        assert "user" in data

    def test_me_endpoint(self, registered_user):
        """Test /auth/me endpoint"""
        response = registered_user.get("/auth/me", auth=True)

        assert response.status_code == 200
        data = response.json()
        assert "id" in data
        assert "email" in data
        assert "display_name" in data

    def test_me_without_token(self, client):
        """Test /auth/me without authentication"""
        response = client.get("/auth/me", auth=False)
        assert response.status_code == 401


class TestTaskManagement:
    """Test task-related endpoints"""

    def test_create_task(self, registered_user):
        """Test creating a new task"""
        task_data = {
            "title": "Test Task",
            "description": "This is a test task",
            "priority": "high",
            "status": "open",
            "estimated_hours": 10.5,
        }

        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 201

        data = response.json()
        assert data["title"] == "Test Task"
        assert data["description"] == "This is a test task"
        assert data["priority"] == "high"
        assert "id" in data

    def test_create_task_minimal(self, registered_user):
        """Test creating task with minimal data (title + description)"""
        task_data = {"title": "Minimal Task", "description": "Minimal description"}

        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 201

        data = response.json()
        assert data["title"] == "Minimal Task"

    def test_get_tasks(self, registered_user):
        """Test getting tasks list"""
        # Create a task first
        task_data = {"title": "Task for List", "description": "Listed task"}
        registered_user.post("/tasks", task_data, auth=True)

        # Get tasks
        response = registered_user.get("/tasks", auth=True)
        assert response.status_code == 200

        data = response.json()
        assert isinstance(data, list)
        assert len(data) > 0

    def test_create_subtask(self, registered_user):
        """Test creating a subtask"""
        # Create parent task
        parent_data = {"title": "Parent Task", "description": "Parent desc"}
        parent_response = registered_user.post("/tasks", parent_data, auth=True)
        parent_id = parent_response.json()["id"]

        # Create subtask
        subtask_data = {
            "title": "Subtask",
            "description": "Subtask desc",
            "parent_task_id": parent_id,
        }
        response = registered_user.post("/tasks", subtask_data, auth=True)
        assert response.status_code == 201

        data = response.json()
        assert data["title"] == "Subtask"
        assert data["parent_task_id"] == parent_id

    def test_update_task(self, registered_user):
        """Test updating a task"""
        # Create task
        task_data = {"title": "Original Title", "description": "Original description"}
        create_response = registered_user.post("/tasks", task_data, auth=True)
        task_id = create_response.json()["id"]

        # Update task
        update_data = {"title": "Updated Title", "status": "in_progress"}
        response = registered_user.put(f"/tasks/{task_id}", update_data, auth=True)
        assert response.status_code == 200

        data = response.json()
        assert data["title"] == "Updated Title"
        assert data["status"] == "in_progress"

    def test_delete_task(self, registered_user):
        """Test deleting a task"""
        # Create task
        task_data = {"title": "Task to Delete", "description": "To be deleted"}
        create_response = registered_user.post("/tasks", task_data, auth=True)
        task_id = create_response.json()["id"]

        # Delete task
        response = registered_user.delete(f"/tasks/{task_id}", auth=True)
        assert response.status_code == 200


class TestTaskAssignments:
    """Test task assignment functionality"""

    def test_create_assignment(self, registered_user):
        """Test creating a task assignment"""
        # Create a task
        task_data = {"title": "Task with Assignment", "description": "Assignment test"}
        task_response = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_response.json()["id"]

        # Try to create assignment for the same user (already exists as owner)
        assignment_data = {"assigned_hours": 5.0}
        response = registered_user.post(
            f"/tasks/{task_id}/assignments", assignment_data, auth=True
        )
        # Should get 409 Conflict because owner is already assigned
        assert response.status_code == 409

    def test_list_assignments(self, registered_user):
        """Test listing task assignments"""
        # Create a task with assignment
        task_data = {
            "title": "Task for Assignment List",
            "description": "Assignment list test",
        }
        task_response = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_response.json()["id"]

        # List assignments
        response = registered_user.get(f"/tasks/{task_id}/assignments", auth=True)
        assert response.status_code == 200

        data = response.json()
        assert isinstance(data, list)


class TestCalendar:
    """Test calendar endpoints"""

    def test_get_calendar_tasks(self, registered_user):
        """Test getting calendar view of tasks"""
        # Create some tasks
        for i in range(3):
            task_data = {
                "title": f"Calendar Task {i}",
                "description": f"Calendar description {i}",
                "start_date": "2024-01-10",
                "due_date": "2024-01-15",
                "assignee_user_id": registered_user.user_id,
            }
            registered_user.post("/tasks", task_data, auth=True)

        # Get calendar tasks
        params = {"start_date": "2024-01-01", "end_date": "2024-01-31"}
        response = registered_user.get("/calendar/tasks", params=params, auth=True)
        assert response.status_code == 200

        data = response.json()
        assert isinstance(data, list)


class TestAuthorization:
    """Test authorization and permissions"""

    def test_unauthorized_access(self, client):
        """Test accessing protected endpoint without token"""
        response = client.get("/tasks", auth=False)
        assert response.status_code == 401

    def test_invalid_token(self, client):
        """Test with invalid token"""
        client.set_token("invalid.token.here", "fake-user-id")
        response = client.get("/tasks", auth=True)
        assert response.status_code == 401


class TestTaskValidation:
    """Test task field validation"""

    def test_create_task_missing_description(self, registered_user):
        """Creating a task without description should fail"""
        task_data = {"title": "No Description Task"}
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_create_task_empty_description(self, registered_user):
        """Creating a task with empty description should fail"""
        task_data = {"title": "Empty Description", "description": ""}
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_create_task_negative_hours(self, registered_user):
        """Creating a task with negative estimated_hours should fail"""
        task_data = {
            "title": "Neg Hours",
            "description": "Test description",
            "estimated_hours": -5,
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_create_task_zero_hours(self, registered_user):
        """Creating a task with 0 estimated_hours should succeed"""
        task_data = {
            "title": "Zero Hours",
            "description": "Test description",
            "estimated_hours": 0,
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 201

    def test_create_task_valid(self, registered_user):
        """Creating a task with valid description and hours"""
        task_data = {
            "title": "Valid Task",
            "description": "This task has a proper description",
            "estimated_hours": 8.5,
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 201
        data = response.json()
        assert data["title"] == "Valid Task"
        assert data["description"] == "This task has a proper description"

    def test_update_task_empty_description(self, registered_user):
        """Updating task description to empty should fail"""
        task_data = {"title": "Task for Update", "description": "Original description"}
        create_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = create_resp.json()["id"]

        update_data = {"description": ""}
        response = registered_user.put(f"/tasks/{task_id}", update_data, auth=True)
        assert response.status_code == 400

    def test_update_task_negative_hours(self, registered_user):
        """Updating estimated_hours to negative should fail"""
        task_data = {"title": "Task for Hour Update", "description": "Some description"}
        create_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = create_resp.json()["id"]

        update_data = {"estimated_hours": -10}
        response = registered_user.put(f"/tasks/{task_id}", update_data, auth=True)
        assert response.status_code == 400


class TestDateValidation:
    """Test task dates vs project dates validation"""

    def test_task_dates_within_project(self, registered_user):
        """Task dates within project range should succeed"""
        # Create project (root task)
        project_data = {
            "title": "Project",
            "description": "Project description",
            "start_date": "2025-01-01",
            "due_date": "2025-12-31",
            "assignee_user_id": registered_user.user_id,
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        assert proj_resp.status_code == 201
        project_id = proj_resp.json()["id"]

        # Create task within project date range
        task_data = {
            "title": "Task in Project",
            "description": "Valid task inside project dates",
            "project_root_id": project_id,
            "start_date": "2025-03-01",
            "due_date": "2025-06-30",
            "assignee_user_id": registered_user.user_id,
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 201

    def test_task_start_before_project(self, registered_user):
        """Task start_date before project start_date should fail"""
        project_data = {
            "title": "Project for Start Check",
            "description": "Project description",
            "start_date": "2025-06-01",
            "due_date": "2025-12-31",
            "assignee_user_id": registered_user.user_id,
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        project_id = proj_resp.json()["id"]

        task_data = {
            "title": "Early Task",
            "description": "Task starting too early",
            "project_root_id": project_id,
            "start_date": "2025-01-01",
            "due_date": "2025-07-01",
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_task_due_after_project(self, registered_user):
        """Task due_date after project due_date should fail"""
        project_data = {
            "title": "Project for Due Check",
            "description": "Project description",
            "start_date": "2025-01-01",
            "due_date": "2025-06-30",
            "assignee_user_id": registered_user.user_id,
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        project_id = proj_resp.json()["id"]

        task_data = {
            "title": "Late Task",
            "description": "Task ending too late",
            "project_root_id": project_id,
            "start_date": "2025-03-01",
            "due_date": "2025-12-31",
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_update_task_dates_violation(self, registered_user):
        """Updating task dates outside project range should fail"""
        project_data = {
            "title": "Project for Update Check",
            "description": "Project description",
            "start_date": "2025-01-01",
            "due_date": "2025-06-30",
            "assignee_user_id": registered_user.user_id,
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        project_id = proj_resp.json()["id"]

        task_data = {
            "title": "Task to Update Dates",
            "description": "Proper description",
            "project_root_id": project_id,
            "start_date": "2025-02-01",
            "due_date": "2025-05-01",
            "assignee_user_id": registered_user.user_id,
        }
        create_resp = registered_user.post("/tasks", task_data, auth=True)
        assert create_resp.status_code == 201
        task_id = create_resp.json()["id"]

        update_data = {"due_date": "2025-12-31"}
        response = registered_user.put(f"/tasks/{task_id}", update_data, auth=True)
        assert response.status_code == 400


class TestSubtasks:
    """Test subtask CRUD functionality"""

    def test_create_subtask(self, registered_user):
        """Create a subtask linked to parent"""
        parent_data = {"title": "Parent Task", "description": "Parent description"}
        parent_resp = registered_user.post("/tasks", parent_data, auth=True)
        assert parent_resp.status_code == 201
        parent_id = parent_resp.json()["id"]

        subtask_data = {
            "title": "Subtask",
            "description": "Subtask description",
            "parent_task_id": parent_id,
        }
        response = registered_user.post("/tasks", subtask_data, auth=True)
        assert response.status_code == 201
        assert response.json()["parent_task_id"] == parent_id

    def test_get_subtasks(self, registered_user):
        """List subtasks of a parent"""
        parent_data = {"title": "Parent for List", "description": "Parent description"}
        parent_resp = registered_user.post("/tasks", parent_data, auth=True)
        parent_id = parent_resp.json()["id"]

        for i in range(3):
            sub_data = {
                "title": f"Subtask {i}",
                "description": f"Sub description {i}",
                "parent_task_id": parent_id,
            }
            registered_user.post("/tasks", sub_data, auth=True)

        response = registered_user.get(f"/tasks/{parent_id}/subtasks", auth=True)
        assert response.status_code == 200
        data = response.json()
        assert isinstance(data, list)
        assert len(data) >= 3
        # Verify all fields are present
        for sub in data:
            assert "id" in sub
            assert "parent_task_id" in sub
            assert "estimated_hours" in sub
            assert "created_at" in sub
            assert "updated_at" in sub

    def test_update_subtask(self, registered_user):
        """Update a subtask"""
        parent_data = {
            "title": "Parent for Update",
            "description": "Parent description",
        }
        parent_resp = registered_user.post("/tasks", parent_data, auth=True)
        parent_id = parent_resp.json()["id"]

        sub_data = {
            "title": "Subtask to Update",
            "description": "Original sub description",
            "parent_task_id": parent_id,
        }
        sub_resp = registered_user.post("/tasks", sub_data, auth=True)
        sub_id = sub_resp.json()["id"]

        update_data = {"title": "Updated Subtask Title", "status": "in_progress"}
        response = registered_user.put(f"/tasks/{sub_id}", update_data, auth=True)
        assert response.status_code == 200
        assert response.json()["title"] == "Updated Subtask Title"

    def test_delete_subtask(self, registered_user):
        """Delete a subtask"""
        parent_data = {
            "title": "Parent for Delete",
            "description": "Parent description",
        }
        parent_resp = registered_user.post("/tasks", parent_data, auth=True)
        parent_id = parent_resp.json()["id"]

        sub_data = {
            "title": "Subtask to Delete",
            "description": "Sub description",
            "parent_task_id": parent_id,
        }
        sub_resp = registered_user.post("/tasks", sub_data, auth=True)
        sub_id = sub_resp.json()["id"]

        response = registered_user.delete(f"/tasks/{sub_id}", auth=True)
        assert response.status_code == 200

    def test_rebind_subtask_parent(self, registered_user):
        """Change parent_task_id of a subtask via PUT"""
        parent1_data = {"title": "Parent 1", "description": "First parent"}
        parent2_data = {"title": "Parent 2", "description": "Second parent"}
        p1_resp = registered_user.post("/tasks", parent1_data, auth=True)
        p2_resp = registered_user.post("/tasks", parent2_data, auth=True)
        p1_id = p1_resp.json()["id"]
        p2_id = p2_resp.json()["id"]

        sub_data = {
            "title": "Rebindable Subtask",
            "description": "Sub description",
            "parent_task_id": p1_id,
        }
        sub_resp = registered_user.post("/tasks", sub_data, auth=True)
        sub_id = sub_resp.json()["id"]

        update_data = {"parent_task_id": p2_id}
        response = registered_user.put(f"/tasks/{sub_id}", update_data, auth=True)
        assert response.status_code == 200
        assert response.json()["parent_task_id"] == p2_id

    def test_invalid_parent_task_id(self, registered_user):
        """Creating subtask with non-existent parent should fail"""
        import uuid

        fake_id = str(uuid.uuid4())
        sub_data = {
            "title": "Orphan Subtask",
            "description": "No parent",
            "parent_task_id": fake_id,
        }
        response = registered_user.post("/tasks", sub_data, auth=True)
        assert response.status_code == 400


class TestTaskNotes:
    """Test task notes CRUD"""

    def test_create_note(self, registered_user):
        """Create a note on a task"""
        task_data = {"title": "Task with Notes", "description": "Task for notes test"}
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        assert task_resp.status_code == 201, task_resp.text
        task_id = task_resp.json()["id"]

        note_data = {"content": "This is a test note"}
        response = registered_user.post(f"/tasks/{task_id}/notes", note_data, auth=True)
        assert response.status_code == 201
        data = response.json()
        assert data["content"] == "This is a test note"
        assert data["task_id"] == task_id
        assert "id" in data
        assert "created_at" in data

    def test_create_note_empty_content(self, registered_user):
        """Creating a note with empty content should fail"""
        task_data = {"title": "Task for Empty Note", "description": "Description"}
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        assert task_resp.status_code == 201, task_resp.text
        task_id = task_resp.json()["id"]

        note_data = {"content": ""}
        response = registered_user.post(f"/tasks/{task_id}/notes", note_data, auth=True)
        assert response.status_code == 400

    def test_list_notes(self, registered_user):
        """List notes on a task"""
        task_data = {"title": "Task for Listing Notes", "description": "Description"}
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        assert task_resp.status_code == 201, task_resp.text
        task_id = task_resp.json()["id"]

        for i in range(3):
            registered_user.post(
                f"/tasks/{task_id}/notes", {"content": f"Note {i}"}, auth=True
            )

        response = registered_user.get(f"/tasks/{task_id}/notes", auth=True)
        assert response.status_code == 200
        data = response.json()
        assert isinstance(data, list)
        assert len(data) >= 3
        for note in data:
            assert "id" in note
            assert "content" in note
            assert "author_name" in note

    def test_update_note(self, registered_user):
        """Update a note"""
        task_data = {"title": "Task for Note Update", "description": "Description"}
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        assert task_resp.status_code == 201, task_resp.text
        task_id = task_resp.json()["id"]

        note_resp = registered_user.post(
            f"/tasks/{task_id}/notes", {"content": "Original note"}, auth=True
        )
        note_id = note_resp.json()["id"]

        update_data = {"content": "Updated note content"}
        response = registered_user.put(f"/notes/{note_id}", update_data, auth=True)
        assert response.status_code == 200
        assert response.json()["content"] == "Updated note content"

    def test_delete_note(self, registered_user):
        """Delete a note"""
        task_data = {"title": "Task for Note Delete", "description": "Description"}
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        assert task_resp.status_code == 201, task_resp.text
        task_id = task_resp.json()["id"]

        note_resp = registered_user.post(
            f"/tasks/{task_id}/notes", {"content": "Note to delete"}, auth=True
        )
        note_id = note_resp.json()["id"]

        response = registered_user.delete(f"/notes/{note_id}", auth=True)
        assert response.status_code == 200

        # Verify deletion
        list_resp = registered_user.get(f"/tasks/{task_id}/notes", auth=True)
        note_ids = [n["id"] for n in list_resp.json()]
        assert note_id not in note_ids


class TestUpdateAllFields:
    """Test editing all task fields via PUT"""

    def test_update_all_fields(self, registered_user):
        """Update all editable fields at once"""
        task_data = {
            "title": "Original",
            "description": "Original description",
            "priority": "low",
            "status": "open",
            "estimated_hours": 5,
            "start_date": "2025-01-01",
            "due_date": "2025-06-30",
            "assignee_user_id": registered_user.user_id,
        }
        create_resp = registered_user.post("/tasks", task_data, auth=True)
        assert create_resp.status_code == 201
        task_id = create_resp.json()["id"]

        update_data = {
            "title": "Updated Title",
            "description": "Updated description",
            "priority": "urgent",
            "status": "in_progress",
            "estimated_hours": 20,
            "start_date": "2025-02-01",
            "due_date": "2025-05-01",
        }
        response = registered_user.put(f"/tasks/{task_id}", update_data, auth=True)
        assert response.status_code == 200
        data = response.json()
        assert data["title"] == "Updated Title"
        assert data["description"] == "Updated description"
        assert data["priority"] == "urgent"
        assert data["status"] == "in_progress"

    def test_update_project_root_id(self, registered_user):
        """Assign task to a project via PUT"""
        project_data = {
            "title": "New Project",
            "description": "Project desc",
            "start_date": "2025-01-01",
            "due_date": "2025-12-31",
            "assignee_user_id": registered_user.user_id,
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        proj_id = proj_resp.json()["id"]

        task_data = {"title": "Unassigned Task", "description": "Task without project"}
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        update_data = {
            "project_root_id": proj_id,
            "start_date": "2025-03-01",
            "due_date": "2025-06-01",
        }
        response = registered_user.put(f"/tasks/{task_id}", update_data, auth=True)
        assert response.status_code == 200
        assert response.json()["project_root_id"] == proj_id

    def test_set_parent_task_id_to_null(self, registered_user):
        """Detach subtask from parent via PUT"""
        parent_data = {"title": "Parent for Detach", "description": "Parent desc"}
        parent_resp = registered_user.post("/tasks", parent_data, auth=True)
        parent_id = parent_resp.json()["id"]

        sub_data = {
            "title": "Subtask for Detach",
            "description": "Sub desc",
            "parent_task_id": parent_id,
        }
        sub_resp = registered_user.post("/tasks", sub_data, auth=True)
        sub_id = sub_resp.json()["id"]

        update_data = {"parent_task_id": None}
        response = registered_user.put(f"/tasks/{sub_id}", update_data, auth=True)
        assert response.status_code == 200
        assert response.json()["parent_task_id"] is None

    def test_update_self_parent_rejected(self, registered_user):
        """Setting task as its own parent should fail"""
        task_data = {"title": "Self Parent", "description": "Test self reference"}
        create_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = create_resp.json()["id"]

        update_data = {"parent_task_id": task_id}
        response = registered_user.put(f"/tasks/{task_id}", update_data, auth=True)
        assert response.status_code == 400


class TestTaskTimeAndAvailability:
    """Test new APIs for time split, availability and workload"""

    def test_task_with_dates_requires_assignee(self, registered_user):
        task_data = {
            "title": "Timed task without assignee",
            "description": "Should fail",
            "start_date": "2026-03-14",
            "due_date": "2026-03-16",
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_task_with_dates_auto_status_in_progress(self, registered_user):
        task_data = {
            "title": "Timed task auto status",
            "description": "Auto in progress",
            "start_date": "2026-03-14",
            "due_date": "2026-03-16",
            "assignee_user_id": registered_user.user_id,
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 201
        assert response.json()["status"] == "in_progress"

    def test_get_tasks_has_time_filter(self, registered_user):
        timed = {
            "title": "Timed for filter",
            "description": "Timed",
            "start_date": "2026-03-20",
            "due_date": "2026-03-21",
            "assignee_user_id": registered_user.user_id,
        }
        plain = {"title": "Untimed for filter", "description": "No dates"}
        r1 = registered_user.post("/tasks", timed, auth=True)
        r2 = registered_user.post("/tasks", plain, auth=True)
        assert r1.status_code == 201
        assert r2.status_code == 201
        timed_id = r1.json()["id"]
        plain_id = r2.json()["id"]

        with_time = registered_user.get(
            "/tasks", params={"has_time": "true"}, auth=True
        )
        assert with_time.status_code == 200
        ids_with_time = {x.get("id") for x in with_time.json()}
        assert timed_id in ids_with_time

        without_time = registered_user.get(
            "/tasks", params={"has_time": "false"}, auth=True
        )
        assert without_time.status_code == 200
        ids_without_time = {x.get("id") for x in without_time.json()}
        assert plain_id in ids_without_time

    def test_calculate_availability_endpoint(self, registered_user):
        payload = {
            "user_id": registered_user.user_id,
            "start_ts": "2026-03-14T09:00:00Z",
            "end_ts": "2026-03-14T18:00:00Z",
        }
        response = registered_user.post(
            "/tasks/availability/calculate", payload, auth=True
        )
        assert response.status_code == 200
        data = response.json()
        assert data["user_id"] == registered_user.user_id
        assert "capacity_hours" in data
        assert "busy_hours" in data
        assert "available_hours" in data

    def test_candidate_assignees_endpoint(self, registered_user):
        task_data = {
            "title": "Task for candidates",
            "description": "Candidates",
            "start_date": "2026-03-18",
            "due_date": "2026-03-19",
            "assignee_user_id": registered_user.user_id,
        }
        create_resp = registered_user.post("/tasks", task_data, auth=True)
        assert create_resp.status_code == 201
        task_id = create_resp.json()["id"]

        payload = {
            "required_skills": ["backend", "sql"],
            "start_ts": "2026-03-18T09:00:00Z",
            "end_ts": "2026-03-19T18:00:00Z",
        }
        response = registered_user.post(
            f"/tasks/{task_id}/candidate-assignees", payload, auth=True
        )
        assert response.status_code == 200
        data = response.json()
        assert data["task_id"] == task_id
        assert isinstance(data["candidates"], list)

    def test_user_workload_endpoint(self, registered_user):
        params = {"start_ts": "2026-03-14T00:00:00Z", "end_ts": "2026-03-21T00:00:00Z"}
        response = registered_user.get(
            f"/users/{registered_user.user_id}/workload", params=params, auth=True
        )
        assert response.status_code == 200
        data = response.json()
        assert data["user_id"] == registered_user.user_id
        assert "scheduled_tasks" in data
        assert "load_percent" in data


class TestProjectsApi:
    """Test project-focused endpoints."""

    @staticmethod
    def _register_extra_user(client: APIClient, prefix: str) -> APIClient:
        import uuid

        email = f"{prefix}_{uuid.uuid4().hex[:8]}@example.com"
        payload = {
            "email": email,
            "password": "TestPassword123!",
            "display_name": f"{prefix} user",
            "work_schedule": [
                {"weekday": 0, "start_time": "09:00:00", "end_time": "18:00:00"}
            ],
        }
        resp = client.post("/auth/register", payload)
        assert resp.status_code == 201
        data = resp.json()

        extra_client = APIClient()
        extra_client.set_token(data["token"], data["user"]["id"])
        return extra_client

    def test_create_project_requires_non_empty_description(self, registered_user):
        payload = {
            "title": "Project without description",
            "description": "",
            "visibility": "private",
        }
        response = registered_user.post("/projects", payload, auth=True)
        assert response.status_code == 400

    def test_create_project_validates_visibility(self, registered_user):
        payload = {
            "title": "Bad visibility project",
            "description": "Should fail",
            "visibility": "internal",
        }
        response = registered_user.post("/projects", payload, auth=True)
        assert response.status_code == 400

    def test_create_project_rejects_multiple_owner_fields(self, registered_user):
        payload = {
            "title": "Bad owner project",
            "description": "Should fail",
            "owners": [registered_user.user_id],
            "visibility": "private",
        }
        response = registered_user.post("/projects", payload, auth=True)
        assert response.status_code == 400

    def test_create_project_and_filter_by_visibility(self, registered_user, client):
        private_payload = {
            "title": "Private Filter Project",
            "description": "Private project",
            "visibility": "private",
        }
        public_payload = {
            "title": "Public Filter Project",
            "description": "Public project",
            "visibility": "public",
        }

        private_resp = registered_user.post("/projects", private_payload, auth=True)
        public_resp = registered_user.post("/projects", public_payload, auth=True)
        assert private_resp.status_code == 201
        assert public_resp.status_code == 201
        assert private_resp.json()["visibility"] == "private"
        assert public_resp.json()["visibility"] == "public"

        # Another user must see only public projects when filtered.
        second_client = self._register_extra_user(client, "project_filter")

        list_resp = second_client.get(
            "/projects", params={"visibility": "public"}, auth=True
        )
        assert list_resp.status_code == 200
        ids = {item["id"] for item in list_resp.json() if "id" in item}
        assert public_resp.json()["id"] in ids
        assert private_resp.json()["id"] not in ids

    def test_project_progress_percent(self, registered_user):
        project_resp = registered_user.post(
            "/projects",
            {
                "title": "Progress Project",
                "description": "Progress calculation",
                "visibility": "private",
            },
            auth=True,
        )
        assert project_resp.status_code == 201
        project_id = project_resp.json()["id"]

        done_task = {
            "title": "Done task",
            "description": "Done",
            "status": "completed",
            "project_root_id": project_id,
        }
        open_task = {
            "title": "Open task",
            "description": "Open",
            "status": "open",
            "project_root_id": project_id,
        }
        r1 = registered_user.post("/tasks", done_task, auth=True)
        r2 = registered_user.post("/tasks", open_task, auth=True)
        assert r1.status_code == 201
        assert r2.status_code == 201

        progress_resp = registered_user.get(
            f"/projects/{project_id}/progress", auth=True
        )
        assert progress_resp.status_code == 200
        progress = progress_resp.json()
        assert progress["project_id"] == project_id
        assert progress["total_tasks"] >= 2
        assert progress["completed_tasks"] >= 1
        assert 0 <= float(progress["progress_percent"]) <= 100

    def test_project_participants_filter_and_sort(self, registered_user):
        project_resp = registered_user.post(
            "/projects",
            {
                "title": "Participants Project",
                "description": "Participants",
                "visibility": "private",
            },
            auth=True,
        )
        assert project_resp.status_code == 201
        project_id = project_resp.json()["id"]

        participants_resp = registered_user.get(
            f"/projects/{project_id}/participants",
            params={"role": "owner", "sort_by": "role", "order": "asc"},
            auth=True,
        )
        assert participants_resp.status_code == 200
        participants = participants_resp.json()
        assert isinstance(participants, list)
        assert len(participants) >= 1
        assert all(item.get("role") == "owner" for item in participants)


class TestOpenApiSpec:
    """Validate OpenAPI specification consistency"""

    def test_openapi_file_exists_and_valid(self):
        spec_path = Path(__file__).resolve().parent.parent / "docs" / "openapi.yaml"
        assert spec_path.exists(), f"Missing OpenAPI file: {spec_path}"

        with spec_path.open("r", encoding="utf-8") as f:
            spec = yaml.safe_load(f)

        assert isinstance(spec, dict)
        assert str(spec.get("openapi", "")).startswith("3.")
        assert "paths" in spec and isinstance(spec["paths"], dict)

    def test_openapi_contains_current_routes(self):
        spec_path = Path(__file__).resolve().parent.parent / "docs" / "openapi.yaml"
        with spec_path.open("r", encoding="utf-8") as f:
            spec = yaml.safe_load(f)

        expected_paths = {
            "/api/auth/register",
            "/api/auth/login",
            "/api/auth/me",
            "/api/projects",
            "/api/projects/{project_id}/progress",
            "/api/projects/{project_id}/participants",
            "/api/tasks",
            "/api/tasks/{task_id}",
            "/api/tasks/{task_id}/subtasks",
            "/api/tasks/{task_id}/assignments",
            "/api/assignments/{assignment_id}",
            "/api/tasks/{task_id}/notes",
            "/api/notes/{note_id}",
            "/api/tasks/availability/calculate",
            "/api/tasks/{task_id}/candidate-assignees",
            "/api/users/{id}/work-schedule",
            "/api/users/{id}/workload",
            "/api/users",
            "/api/users/{id}",
            "/api/calendar/tasks",
        }

        paths = set(spec.get("paths", {}).keys())
        missing = expected_paths - paths
        assert not missing, f"OpenAPI is missing routes: {sorted(missing)}"


class TestAuthValidation:
    """Edge cases for registration and login field validation"""

    def _base_payload(self) -> dict:
        import uuid

        return {
            "email": f"edge_{uuid.uuid4().hex[:8]}@example.com",
            "password": "SecurePass123!",
            "display_name": "Edge User",
            "work_schedule": [
                {"weekday": 1, "start_time": "09:00:00", "end_time": "17:00:00"}
            ],
        }

    def test_register_missing_email(self, client):
        """Registration without email field must fail"""
        payload = self._base_payload()
        del payload["email"]
        response = client.post("/auth/register", payload)
        assert response.status_code == 400

    def test_register_missing_display_name(self, client):
        """Registration without display_name succeeds — backend derives it from email"""
        payload = self._base_payload()
        del payload["display_name"]
        response = client.post("/auth/register", payload)
        assert response.status_code == 201, response.text
        # display_name is derived from the email local part
        assert response.json()["user"]["display_name"] != ""

    def test_register_missing_password(self, client):
        """Registration without password must fail"""
        payload = self._base_payload()
        del payload["password"]
        response = client.post("/auth/register", payload)
        assert response.status_code == 400

    def test_register_invalid_email_no_at(self, client):
        """Registration with email missing @ must fail"""
        payload = self._base_payload()
        payload["email"] = "notanemail"
        response = client.post("/auth/register", payload)
        assert response.status_code == 400

    def test_register_invalid_email_no_domain(self, client):
        """Registration with email like user@ must fail"""
        payload = self._base_payload()
        payload["email"] = "user@"
        response = client.post("/auth/register", payload)
        assert response.status_code == 400

    def test_register_weekday_negative_one(self, client):
        """Registration with weekday=-1 must not succeed (invalid day)"""
        payload = self._base_payload()
        payload["work_schedule"] = [
            {"weekday": -1, "start_time": "09:00:00", "end_time": "17:00:00"}
        ]
        response = client.post("/auth/register", payload)
        assert response.status_code != 201, "weekday=-1 should be rejected"

    def test_register_weekday_eight(self, client):
        """Registration with weekday=8 must not succeed (out of range)"""
        payload = self._base_payload()
        payload["work_schedule"] = [
            {"weekday": 8, "start_time": "09:00:00", "end_time": "17:00:00"}
        ]
        response = client.post("/auth/register", payload)
        assert response.status_code != 201, "weekday=8 should be rejected"

    def test_register_all_weekdays_zero_to_six(self, client):
        """Registration with all weekdays 0-6 must succeed"""
        payload = self._base_payload()
        payload["work_schedule"] = [
            {"weekday": d, "start_time": "09:00:00", "end_time": "17:00:00"}
            for d in range(7)
        ]
        response = client.post("/auth/register", payload)
        assert response.status_code == 201, response.text

    def test_register_full_iso_week_one_to_seven(self, client):
        """Registration with weekdays 1-7 (ISO, 7=Sunday) must succeed"""
        payload = self._base_payload()
        payload["work_schedule"] = [
            {"weekday": d, "start_time": "09:00:00", "end_time": "17:00:00"}
            for d in range(1, 8)
        ]
        response = client.post("/auth/register", payload)
        assert response.status_code == 201, response.text

    def test_register_empty_work_schedule(self, client):
        """Registration with empty work_schedule array must succeed"""
        payload = self._base_payload()
        payload["work_schedule"] = []
        response = client.post("/auth/register", payload)
        assert response.status_code == 201, response.text

    def test_register_no_work_schedule_field(self, client):
        """Registration without work_schedule field must succeed (defaults to empty)"""
        payload = self._base_payload()
        del payload["work_schedule"]
        response = client.post("/auth/register", payload)
        assert response.status_code == 201, response.text

    def test_register_duplicate_weekday_entries(self, client):
        """Registration with two entries for the same weekday"""
        payload = self._base_payload()
        payload["work_schedule"] = [
            {"weekday": 1, "start_time": "09:00:00", "end_time": "13:00:00"},
            {"weekday": 1, "start_time": "14:00:00", "end_time": "18:00:00"},
        ]
        response = client.post("/auth/register", payload)
        # Either accepted (two shifts same day) or rejected (duplicate day)
        assert response.status_code in (201, 400), response.text

    def test_login_wrong_password(self, client):
        """Login with wrong password must return 401"""
        import uuid

        email = f"wrongpw_{uuid.uuid4().hex[:8]}@example.com"
        reg_payload = {
            "email": email,
            "password": "CorrectPass123!",
            "display_name": "WrongPW User",
            "work_schedule": [],
        }
        r = client.post("/auth/register", reg_payload)
        assert r.status_code == 201

        response = client.post(
            "/auth/login", {"email": email, "password": "WrongPass!"}
        )
        assert response.status_code == 401

    def test_login_nonexistent_user(self, client):
        """Login for email that was never registered must return 401 or 404"""
        response = client.post(
            "/auth/login",
            {"email": "nobody_at_all@nowhere.example.com", "password": "Pass123!"},
        )
        assert response.status_code in (401, 404)

    def test_login_missing_email(self, client):
        """Login without email must fail"""
        response = client.post("/auth/login", {"password": "Pass123!"})
        assert response.status_code == 400

    def test_login_missing_password(self, client):
        """Login without password must fail"""
        response = client.post("/auth/login", {"email": "user@example.com"})
        assert response.status_code == 400


class TestWorkSchedule:
    """Edge cases for GET/POST /users/{id}/work-schedule.

    The API uses POST (replace-all semantics) with day_of_week 1-7 and
    is_working_day flag. Time format is HH:MM (not HH:MM:SS).
    Array must have 1-7 elements.
    """

    def _ws_payload(self, *days):
        """Build a valid work-schedule payload for given day_of_week values (1-7)."""
        return [
            {
                "day_of_week": d,
                "is_working_day": True,
                "start_time": "09:00",
                "end_time": "17:00",
            }
            for d in days
        ]

    def test_get_own_work_schedule(self, registered_user):
        """Owner can read their work schedule"""
        response = registered_user.get(
            f"/users/{registered_user.user_id}/work-schedule", auth=True
        )
        assert response.status_code == 200
        data = response.json()
        assert isinstance(data, list)
        for entry in data:
            assert "day_of_week" in entry
            assert "is_working_day" in entry

    def test_post_work_schedule_valid(self, registered_user):
        """POST replaces the work schedule with valid entries"""
        payload = self._ws_payload(1, 3)
        response = registered_user.post(
            f"/users/{registered_user.user_id}/work-schedule", payload, auth=True
        )
        assert response.status_code in (200, 201), response.text

    def test_post_work_schedule_sunday(self, registered_user):
        """POST with day_of_week=7 (Sunday) must be accepted"""
        payload = self._ws_payload(7)
        response = registered_user.post(
            f"/users/{registered_user.user_id}/work-schedule", payload, auth=True
        )
        assert response.status_code in (200, 201), response.text

    def test_post_work_schedule_day_out_of_range_low(self, registered_user):
        """POST with day_of_week=0 must be rejected (valid range is 1-7)"""
        payload = [
            {
                "day_of_week": 0,
                "is_working_day": True,
                "start_time": "09:00",
                "end_time": "17:00",
            }
        ]
        response = registered_user.post(
            f"/users/{registered_user.user_id}/work-schedule", payload, auth=True
        )
        assert response.status_code != 200, "day_of_week=0 should not be accepted"

    def test_post_work_schedule_day_out_of_range_high(self, registered_user):
        """POST with day_of_week=8 must be rejected"""
        payload = [
            {
                "day_of_week": 8,
                "is_working_day": True,
                "start_time": "09:00",
                "end_time": "17:00",
            }
        ]
        response = registered_user.post(
            f"/users/{registered_user.user_id}/work-schedule", payload, auth=True
        )
        assert response.status_code != 200, "day_of_week=8 should not be accepted"

    def test_post_work_schedule_end_before_start(self, registered_user):
        """POST where end_time < start_time must be rejected"""
        payload = [
            {
                "day_of_week": 3,
                "is_working_day": True,
                "start_time": "17:00",
                "end_time": "09:00",
            }
        ]
        response = registered_user.post(
            f"/users/{registered_user.user_id}/work-schedule", payload, auth=True
        )
        assert response.status_code == 400, response.text

    def test_post_work_schedule_unauthenticated(self, client):
        """POST work-schedule without token must fail with 401"""
        import uuid

        payload = [
            {
                "day_of_week": 1,
                "is_working_day": True,
                "start_time": "09:00",
                "end_time": "17:00",
            }
        ]
        response = client.post(
            f"/users/{uuid.uuid4()}/work-schedule", payload, auth=False
        )
        assert response.status_code == 401

    def test_post_work_schedule_all_days(self, registered_user):
        """POST with all seven days (1-7) must succeed"""
        payload = self._ws_payload(1, 2, 3, 4, 5, 6, 7)
        response = registered_user.post(
            f"/users/{registered_user.user_id}/work-schedule", payload, auth=True
        )
        assert response.status_code in (200, 201), response.text

    def test_post_work_schedule_non_working_day(self, registered_user):
        """POST with is_working_day=false is accepted (no times needed)"""
        payload = [
            {"day_of_week": 6, "is_working_day": False},
            {"day_of_week": 7, "is_working_day": False},
            {
                "day_of_week": 1,
                "is_working_day": True,
                "start_time": "09:00",
                "end_time": "17:00",
            },
        ]
        response = registered_user.post(
            f"/users/{registered_user.user_id}/work-schedule", payload, auth=True
        )
        assert response.status_code in (200, 201), response.text


class TestUsers:
    """User search and profile endpoints"""

    def test_search_users_returns_list(self, registered_user):
        """GET /users?search=... returns a list"""
        response = registered_user.get("/users", params={"search": "Test"}, auth=True)
        assert response.status_code == 200
        assert isinstance(response.json(), list)

    def test_search_users_empty_query(self, registered_user):
        """GET /users without search param returns users or empty list"""
        response = registered_user.get("/users", auth=True)
        assert response.status_code in (200, 400)

    def test_get_user_by_id(self, registered_user):
        """GET /users/{id} for existing user returns profile"""
        response = registered_user.get(f"/users/{registered_user.user_id}", auth=True)
        assert response.status_code == 200
        data = response.json()
        assert data["id"] == registered_user.user_id
        assert "email" in data
        assert "display_name" in data

    def test_get_nonexistent_user(self, registered_user):
        """GET /users/{id} for unknown UUID returns 404"""
        import uuid

        response = registered_user.get(f"/users/{uuid.uuid4()}", auth=True)
        assert response.status_code == 404

    def test_get_user_malformed_id(self, registered_user):
        """GET /users/{id} with non-UUID id returns 400 or 404"""
        response = registered_user.get("/users/not-a-uuid", auth=True)
        assert response.status_code in (400, 404)

    def test_get_user_unauthenticated(self, client):
        """GET /users/{id} is public — returns 200 or 404 (not 401)"""
        import uuid

        response = client.get(f"/users/{uuid.uuid4()}", auth=False)
        assert response.status_code in (200, 404)

    def test_search_finds_registered_user(self, registered_user):
        """Search by email (unique per test) should find the registered user"""
        me_resp = registered_user.get("/auth/me", auth=True)
        email = me_resp.json()["email"]
        # Use the unique local part before '@' — guaranteed unique across test runs
        unique_prefix = email.split("@")[0]

        response = registered_user.get(
            "/users", params={"search": unique_prefix}, auth=True
        )
        assert response.status_code == 200
        ids = [u["id"] for u in response.json()]
        assert registered_user.user_id in ids


class TestTaskEdgeCases:
    """404 and enum validation for task endpoints"""

    def test_get_nonexistent_task(self, registered_user):
        """GET /tasks/{id} for unknown UUID returns 404"""
        import uuid

        response = registered_user.get(f"/tasks/{uuid.uuid4()}", auth=True)
        assert response.status_code == 404

    def test_update_nonexistent_task(self, registered_user):
        """PUT /tasks/{id} for unknown UUID returns 404"""
        import uuid

        response = registered_user.put(
            f"/tasks/{uuid.uuid4()}", {"title": "Ghost"}, auth=True
        )
        assert response.status_code == 404

    def test_delete_nonexistent_task(self, registered_user):
        """DELETE /tasks/{id} for unknown UUID returns 404"""
        import uuid

        response = registered_user.delete(f"/tasks/{uuid.uuid4()}", auth=True)
        assert response.status_code == 404

    def test_get_task_malformed_id(self, registered_user):
        """GET /tasks/{id} with non-UUID returns 400 or 404"""
        response = registered_user.get("/tasks/not-a-uuid", auth=True)
        assert response.status_code in (400, 404)

    def test_create_task_missing_title(self, registered_user):
        """Creating a task without title must fail"""
        response = registered_user.post(
            "/tasks", {"description": "No title here"}, auth=True
        )
        assert response.status_code == 400

    def test_create_task_invalid_priority(self, registered_user):
        """Creating a task with an unknown priority value must fail"""
        task_data = {
            "title": "Bad priority",
            "description": "Test description",
            "priority": "superurgent",
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_create_task_invalid_status(self, registered_user):
        """Creating a task with an unknown status value must fail"""
        task_data = {
            "title": "Bad status",
            "description": "Test description",
            "status": "maybe",
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_create_task_start_after_due(self, registered_user):
        """Task where start_date > due_date must fail"""
        task_data = {
            "title": "Reversed dates",
            "description": "start after due",
            "start_date": "2025-12-01",
            "due_date": "2025-01-01",
            "assignee_user_id": registered_user.user_id,
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_create_task_same_start_and_due(self, registered_user):
        """Task where start_date == due_date must succeed (one-day task)"""
        task_data = {
            "title": "One-day task",
            "description": "Starts and ends same day",
            "start_date": "2025-06-15",
            "due_date": "2025-06-15",
            "assignee_user_id": registered_user.user_id,
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 201, response.text

    def test_list_tasks_unauthenticated(self, client):
        """GET /tasks without token must return 401"""
        response = client.get("/tasks", auth=False)
        assert response.status_code == 401

    def test_create_task_unauthenticated(self, client):
        """POST /tasks without token must return 401"""
        response = client.post(
            "/tasks",
            {"title": "Unauthorized", "description": "Should fail"},
            auth=False,
        )
        assert response.status_code == 401


class TestCalendarEdgeCases:
    """Edge cases for GET /calendar/tasks"""

    def test_calendar_missing_start_date(self, registered_user):
        """Calendar endpoint without start_date must fail"""
        response = registered_user.get(
            "/calendar/tasks", params={"end_date": "2025-12-31"}, auth=True
        )
        assert response.status_code == 400

    def test_calendar_missing_end_date(self, registered_user):
        """Calendar endpoint without end_date must fail"""
        response = registered_user.get(
            "/calendar/tasks", params={"start_date": "2025-01-01"}, auth=True
        )
        assert response.status_code == 400

    def test_calendar_missing_both_dates(self, registered_user):
        """Calendar endpoint without any date params must fail"""
        response = registered_user.get("/calendar/tasks", auth=True)
        assert response.status_code == 400

    def test_calendar_invalid_date_format(self, registered_user):
        """Calendar endpoint with invalid date format must fail"""
        response = registered_user.get(
            "/calendar/tasks",
            params={"start_date": "not-a-date", "end_date": "2025-12-31"},
            auth=True,
        )
        assert response.status_code == 400

    def test_calendar_end_before_start(self, registered_user):
        """Calendar endpoint where end < start must fail or return empty"""
        response = registered_user.get(
            "/calendar/tasks",
            params={"start_date": "2025-12-31", "end_date": "2025-01-01"},
            auth=True,
        )
        # Either 400 (validation) or 200 with empty list
        assert response.status_code in (200, 400)
        if response.status_code == 200:
            assert isinstance(response.json(), list)

    def test_calendar_large_range(self, registered_user):
        """Calendar endpoint with a multi-year range must succeed"""
        response = registered_user.get(
            "/calendar/tasks",
            params={"start_date": "2020-01-01", "end_date": "2030-12-31"},
            auth=True,
        )
        assert response.status_code == 200
        assert isinstance(response.json(), list)

    def test_calendar_unauthenticated(self, client):
        """Calendar endpoint without token must return 401"""
        response = client.get(
            "/calendar/tasks",
            params={"start_date": "2025-01-01", "end_date": "2025-12-31"},
            auth=False,
        )
        assert response.status_code == 401


class TestAssignmentEdgeCases:
    """Edge cases for assignment management"""

    def _make_second_user(self, client: "APIClient") -> "APIClient":
        import uuid

        email = f"assign2_{uuid.uuid4().hex[:8]}@example.com"
        payload = {
            "email": email,
            "password": "TestPass123!",
            "display_name": "Second Assignee",
            "work_schedule": [],
        }
        r = client.post("/auth/register", payload)
        assert r.status_code == 201
        data = r.json()
        c2 = APIClient()
        c2.set_token(data["token"], data["user"]["id"])
        return c2

    def test_delete_assignment(self, registered_user, client):
        """Owner can delete another user's assignment via DELETE /tasks/{task_id}/assignments/{user_id}"""
        c2 = self._make_second_user(client)
        task_data = {"title": "Task for del-assign", "description": "Assign test"}
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        assert task_resp.status_code == 201
        task_id = task_resp.json()["id"]

        # Assign second user
        assign_resp = registered_user.post(
            f"/tasks/{task_id}/assignments",
            {"user_id": c2.user_id, "assigned_hours": 4.0},
            auth=True,
        )
        if assign_resp.status_code == 201:
            response = registered_user.delete(
                f"/tasks/{task_id}/assignments/{c2.user_id}",
                auth=True,
            )
            assert response.status_code == 200

    def test_list_assignments_nonexistent_task(self, registered_user):
        """GET /tasks/{id}/assignments for unknown task returns 403 or 404"""
        import uuid

        response = registered_user.get(f"/tasks/{uuid.uuid4()}/assignments", auth=True)
        # Backend checks membership before existence, so 403 is also valid
        assert response.status_code in (403, 404)

    def test_assign_to_nonexistent_task(self, registered_user):
        """POST /tasks/{id}/assignments for unknown task returns 404"""
        import uuid

        response = registered_user.post(
            f"/tasks/{uuid.uuid4()}/assignments",
            {"assigned_hours": 2.0},
            auth=True,
        )
        assert response.status_code in (404, 400)

    def test_delete_assignment_missing_params(self, registered_user):
        """DELETE /assignments/{id} without a valid id returns 400 or 404"""
        # Route is /api/assignments/{assignment_id} — no bare /assignments route
        response = registered_user.delete("/assignments", auth=True)
        assert response.status_code in (400, 404)


class TestNotificationSettings:
    """GET/PUT /users/{id}/notification-settings"""

    def _url(self, user_id: str) -> str:
        return f"/users/{user_id}/notification-settings"

    def test_get_returns_defaults(self, registered_user):
        """Fresh account: GET returns defaults (enabled=true, days={1,3,7})"""
        response = registered_user.get(self._url(registered_user.user_id), auth=True)
        assert response.status_code == 200, response.text
        data = response.json()
        assert "deadline_reminders_enabled" in data
        assert "reminder_days_before" in data
        assert "reminder_hours_before" in data
        assert isinstance(data["reminder_days_before"], list)
        assert isinstance(data["reminder_hours_before"], list)

    def test_put_saves_settings(self, registered_user):
        """PUT persists all three fields"""
        payload = {
            "deadline_reminders_enabled": True,
            "reminder_days_before": [1, 7],
            "reminder_hours_before": [2, 6],
        }
        response = registered_user.put(
            self._url(registered_user.user_id), payload, auth=True
        )
        assert response.status_code in (200, 204), response.text

    def test_get_after_put_returns_saved(self, registered_user):
        """GET reflects values saved via PUT"""
        payload = {
            "deadline_reminders_enabled": False,
            "reminder_days_before": [3, 14],
            "reminder_hours_before": [1],
        }
        registered_user.put(self._url(registered_user.user_id), payload, auth=True)
        response = registered_user.get(self._url(registered_user.user_id), auth=True)
        assert response.status_code == 200, response.text
        data = response.json()
        assert data["deadline_reminders_enabled"] is False
        assert sorted(data["reminder_days_before"]) == [3, 14]
        assert data["reminder_hours_before"] == [1]

    def test_put_disable_reminders(self, registered_user):
        """PUT with enabled=false must be stored and returned correctly"""
        payload = {
            "deadline_reminders_enabled": False,
            "reminder_days_before": [],
            "reminder_hours_before": [],
        }
        put_resp = registered_user.put(
            self._url(registered_user.user_id), payload, auth=True
        )
        assert put_resp.status_code in (200, 204), put_resp.text

        get_resp = registered_user.get(self._url(registered_user.user_id), auth=True)
        assert get_resp.status_code == 200
        assert get_resp.json()["deadline_reminders_enabled"] is False

    def test_put_empty_arrays(self, registered_user):
        """PUT with empty arrays is valid"""
        payload = {
            "deadline_reminders_enabled": True,
            "reminder_days_before": [],
            "reminder_hours_before": [],
        }
        response = registered_user.put(
            self._url(registered_user.user_id), payload, auth=True
        )
        assert response.status_code in (200, 204), response.text

    def test_put_idempotent(self, registered_user):
        """Calling PUT twice with same data must succeed both times"""
        payload = {
            "deadline_reminders_enabled": True,
            "reminder_days_before": [1, 3],
            "reminder_hours_before": [6],
        }
        r1 = registered_user.put(self._url(registered_user.user_id), payload, auth=True)
        r2 = registered_user.put(self._url(registered_user.user_id), payload, auth=True)
        assert r1.status_code in (200, 204), r1.text
        assert r2.status_code in (200, 204), r2.text

    def test_get_unauthenticated(self, client):
        """GET without token must return 401"""
        import uuid

        response = client.get(self._url(str(uuid.uuid4())), auth=False)
        assert response.status_code == 401

    def test_put_unauthenticated(self, client):
        """PUT without token must return 401"""
        import uuid

        payload = {
            "deadline_reminders_enabled": True,
            "reminder_days_before": [1],
            "reminder_hours_before": [],
        }
        response = client.put(self._url(str(uuid.uuid4())), payload, auth=False)
        assert response.status_code == 401

    def test_get_other_user_forbidden(self, registered_user, client):
        """User A cannot read User B's notification settings"""
        import uuid

        other_email = f"other_{uuid.uuid4().hex[:8]}@example.com"
        other_payload = {
            "email": other_email,
            "password": "Pass1234!",
            "username": f"other_{uuid.uuid4().hex[:6]}",
            "last_name": "Other",
            "first_name": "User",
            "timezone": "UTC",
            "contacts_visible": False,
            "stack": [],
            "work_schedule": [],
        }
        reg_resp = client.post("/auth/register", other_payload)
        assert reg_resp.status_code == 201
        other_id = reg_resp.json()["user"]["id"]

        response = registered_user.get(self._url(other_id), auth=True)
        assert response.status_code in (403, 404)

    def test_put_other_user_forbidden(self, registered_user, client):
        """User A cannot overwrite User B's notification settings"""
        import uuid

        other_email = f"other2_{uuid.uuid4().hex[:8]}@example.com"
        other_payload = {
            "email": other_email,
            "password": "Pass1234!",
            "username": f"other2_{uuid.uuid4().hex[:6]}",
            "last_name": "Other",
            "first_name": "User",
            "timezone": "UTC",
            "contacts_visible": False,
            "stack": [],
            "work_schedule": [],
        }
        reg_resp = client.post("/auth/register", other_payload)
        assert reg_resp.status_code == 201
        other_id = reg_resp.json()["user"]["id"]

        payload = {
            "deadline_reminders_enabled": False,
            "reminder_days_before": [],
            "reminder_hours_before": [],
        }
        response = registered_user.put(self._url(other_id), payload, auth=True)
        assert response.status_code in (403, 404)


class TestGetTaskById:
    """GET /tasks/{task_id} — endpoint added in PR #38"""

    def test_get_task_by_id_returns_task(self, registered_user):
        task_data = {"title": "Get By ID Task", "description": "Single task fetch"}
        create_resp = registered_user.post("/tasks", task_data, auth=True)
        assert create_resp.status_code == 201
        task_id = create_resp.json()["id"]

        response = registered_user.get(f"/tasks/{task_id}", auth=True)
        assert response.status_code == 200
        data = response.json()
        assert data["id"] == task_id
        assert data["title"] == "Get By ID Task"

    def test_get_task_by_id_not_found(self, registered_user):
        import uuid

        response = registered_user.get(f"/tasks/{uuid.uuid4()}", auth=True)
        assert response.status_code in (403, 404)

    def test_get_task_by_id_unauthenticated(self, client, registered_user):
        task_data = {"title": "Auth Check Task", "description": "Should require auth"}
        create_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = create_resp.json()["id"]

        response = client.get(f"/tasks/{task_id}", auth=False)
        assert response.status_code == 401


class TestApplyApproveWorkflow:
    """Full apply/approve/reject workflow for project applications"""

    @staticmethod
    def _make_user(client: "APIClient", prefix: str) -> "APIClient":
        import uuid

        email = f"{prefix}_{uuid.uuid4().hex[:8]}@example.com"
        payload = {
            "email": email,
            "password": "TestPass123!",
            "display_name": f"{prefix} User",
            "work_schedule": [],
        }
        r = client.post("/auth/register", payload)
        assert r.status_code == 201
        data = r.json()
        c = APIClient()
        c.set_token(data["token"], data["user"]["id"])
        return c

    def test_apply_to_project(self, registered_user, client):
        """Non-owner can apply to a project"""
        applicant = self._make_user(client, "applicant")

        project_data = {
            "title": "Open Project",
            "description": "Anyone can apply",
            "start_date": "2026-06-01",
            "due_date": "2026-08-31",
            "assignee_user_id": registered_user.user_id,
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        assert proj_resp.status_code == 201
        project_id = proj_resp.json()["id"]

        apply_resp = applicant.post(f"/projects/{project_id}/apply", {}, auth=True)
        assert apply_resp.status_code in (200, 201, 204)

    def test_owner_sees_applications(self, registered_user, client):
        """Owner can list incoming applications"""
        applicant = self._make_user(client, "applist")

        project_data = {
            "title": "Project With Apps",
            "description": "Check apps list",
            "start_date": "2026-06-01",
            "due_date": "2026-08-31",
            "assignee_user_id": registered_user.user_id,
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        assert proj_resp.status_code == 201
        project_id = proj_resp.json()["id"]

        applicant.post(f"/projects/{project_id}/apply", {}, auth=True)

        apps_resp = registered_user.get(
            f"/projects/{project_id}/applications", auth=True
        )
        assert apps_resp.status_code == 200
        apps = apps_resp.json()
        assert isinstance(apps, list)
        applicant_ids = [a.get("user_id") or a.get("applicant_id") for a in apps]
        assert applicant.user_id in applicant_ids

    def test_owner_approves_application(self, registered_user, client):
        """Owner can approve an application; applicant becomes a member"""
        applicant = self._make_user(client, "approve")

        project_data = {
            "title": "Approving Project",
            "description": "Approve workflow",
            "start_date": "2026-06-01",
            "due_date": "2026-08-31",
            "assignee_user_id": registered_user.user_id,
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        assert proj_resp.status_code == 201
        project_id = proj_resp.json()["id"]

        applicant.post(f"/projects/{project_id}/apply", {}, auth=True)

        apps_resp = registered_user.get(
            f"/projects/{project_id}/applications", auth=True
        )
        assert apps_resp.status_code == 200
        apps = apps_resp.json()
        assert len(apps) >= 1

        app_id = apps[0].get("id") or apps[0].get("application_id")
        approve_resp = registered_user.post(
            f"/projects/{project_id}/applications/{app_id}/approve", {}, auth=True
        )
        assert approve_resp.status_code in (200, 201, 204)

        # Applicant should now appear in assignments
        members_resp = registered_user.get(
            f"/tasks/{project_id}/assignments", auth=True
        )
        assert members_resp.status_code == 200
        member_ids = [m.get("user_id") for m in members_resp.json()]
        assert applicant.user_id in member_ids

    def test_owner_rejects_application(self, registered_user, client):
        """Owner can reject an application; applicant is NOT added"""
        applicant = self._make_user(client, "reject")

        project_data = {
            "title": "Rejecting Project",
            "description": "Reject workflow",
            "start_date": "2026-06-01",
            "due_date": "2026-08-31",
            "assignee_user_id": registered_user.user_id,
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        assert proj_resp.status_code == 201
        project_id = proj_resp.json()["id"]

        applicant.post(f"/projects/{project_id}/apply", {}, auth=True)

        apps_resp = registered_user.get(
            f"/projects/{project_id}/applications", auth=True
        )
        apps = apps_resp.json()
        app_id = apps[0].get("id") or apps[0].get("application_id")

        reject_resp = registered_user.post(
            f"/projects/{project_id}/applications/{app_id}/reject", {}, auth=True
        )
        assert reject_resp.status_code in (200, 201, 204)

        members_resp = registered_user.get(
            f"/tasks/{project_id}/assignments", auth=True
        )
        member_ids = [m.get("user_id") for m in members_resp.json()]
        assert applicant.user_id not in member_ids

    def test_non_owner_cannot_see_applications(self, registered_user, client):
        """Non-owner gets 403 when listing applications"""
        other = self._make_user(client, "nonapps")

        project_data = {
            "title": "Private Apps Project",
            "description": "Only owner sees apps",
            "start_date": "2026-06-01",
            "due_date": "2026-08-31",
            "assignee_user_id": registered_user.user_id,
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        project_id = proj_resp.json()["id"]

        apps_resp = other.get(f"/projects/{project_id}/applications", auth=True)
        assert apps_resp.status_code in (403, 404)


class TestCrossProjectScheduling:
    """Verify that task_schedule is populated on assignment and tracks time cross-project"""

    @staticmethod
    def _make_user(client: "APIClient", prefix: str) -> "APIClient":
        import uuid

        email = f"{prefix}_{uuid.uuid4().hex[:8]}@example.com"
        payload = {
            "email": email,
            "password": "TestPass123!",
            "display_name": f"{prefix} Worker",
            "work_schedule": [
                {"weekday": 0, "start_time": "09:00:00", "end_time": "18:00:00"},
                {"weekday": 1, "start_time": "09:00:00", "end_time": "18:00:00"},
                {"weekday": 2, "start_time": "09:00:00", "end_time": "18:00:00"},
                {"weekday": 3, "start_time": "09:00:00", "end_time": "18:00:00"},
                {"weekday": 4, "start_time": "09:00:00", "end_time": "18:00:00"},
            ],
        }
        r = client.post("/auth/register", payload)
        assert r.status_code == 201
        data = r.json()
        c = APIClient()
        c.set_token(data["token"], data["user"]["id"])
        return c

    def test_task_schedule_populated_on_assignment(self, registered_user, client):
        """After assigning a user to a timed task, availability returns non-zero busy_hours"""
        worker = self._make_user(client, "sched_worker")

        task_data = {
            "title": "Scheduled Task",
            "description": "Has dates and assigned hours",
            "start_date": "2026-07-01",
            "due_date": "2026-07-10",
            "assignee_user_id": registered_user.user_id,
            "estimated_hours": 40,
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        assert task_resp.status_code == 201
        task_id = task_resp.json()["id"]

        # Assign worker to the task
        assign_resp = registered_user.post(
            f"/tasks/{task_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 40.0},
            auth=True,
        )
        assert assign_resp.status_code == 201

        # Check worker's availability — should show busy_hours > 0
        avail_payload = {
            "user_id": worker.user_id,
            "start_ts": "2026-07-01T00:00:00Z",
            "end_ts": "2026-07-10T23:59:59Z",
        }
        avail_resp = registered_user.post(
            "/tasks/availability/calculate", avail_payload, auth=True
        )
        assert avail_resp.status_code == 200
        data = avail_resp.json()
        assert data["busy_hours"] > 0, (
            f"Expected busy_hours > 0 after assignment, got {data['busy_hours']}"
        )

    def test_task_schedule_cleared_on_unassignment(self, registered_user, client):
        """After removing a user's assignment, busy_hours drops to 0 for that task"""
        worker = self._make_user(client, "unsched_worker")

        task_data = {
            "title": "Clearable Task",
            "description": "Schedule cleared on delete",
            "start_date": "2026-07-15",
            "due_date": "2026-07-20",
            "assignee_user_id": registered_user.user_id,
            "estimated_hours": 20,
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        assign_resp = registered_user.post(
            f"/tasks/{task_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 20.0},
            auth=True,
        )
        assert assign_resp.status_code == 201

        # Delete the assignment
        del_resp = registered_user.delete(
            f"/tasks/{task_id}/assignments/{worker.user_id}", auth=True
        )
        assert del_resp.status_code == 200

        # busy_hours should now be 0
        avail_payload = {
            "user_id": worker.user_id,
            "start_ts": "2026-07-15T00:00:00Z",
            "end_ts": "2026-07-20T23:59:59Z",
        }
        avail_resp = registered_user.post(
            "/tasks/availability/calculate", avail_payload, auth=True
        )
        assert avail_resp.status_code == 200
        assert avail_resp.json()["busy_hours"] == 0.0

    def test_cross_project_busy_hours_accumulate(self, registered_user, client):
        """User assigned to two overlapping tasks in different projects shows summed busy_hours"""
        worker = self._make_user(client, "cross_proj_worker")

        # Project 1 task: July 1–10, 40h
        t1_resp = registered_user.post(
            "/tasks",
            {
                "title": "Cross Proj Task 1",
                "description": "Project 1 task",
                "start_date": "2026-07-01",
                "due_date": "2026-07-10",
                "assignee_user_id": registered_user.user_id,
                "estimated_hours": 40,
            },
            auth=True,
        )
        assert t1_resp.status_code == 201
        task1_id = t1_resp.json()["id"]

        # Project 2 task: July 5–15, 40h
        t2_resp = registered_user.post(
            "/tasks",
            {
                "title": "Cross Proj Task 2",
                "description": "Project 2 task",
                "start_date": "2026-07-05",
                "due_date": "2026-07-15",
                "assignee_user_id": registered_user.user_id,
                "estimated_hours": 40,
            },
            auth=True,
        )
        assert t2_resp.status_code == 201
        task2_id = t2_resp.json()["id"]

        # Assign worker to both
        r1 = registered_user.post(
            f"/tasks/{task1_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 40.0},
            auth=True,
        )
        r2 = registered_user.post(
            f"/tasks/{task2_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 40.0},
            auth=True,
        )
        assert r1.status_code == 201
        assert r2.status_code == 201

        # Check availability in the overlap window (July 5–10)
        avail_payload = {
            "user_id": worker.user_id,
            "start_ts": "2026-07-05T00:00:00Z",
            "end_ts": "2026-07-10T23:59:59Z",
        }
        avail_resp = registered_user.post(
            "/tasks/availability/calculate", avail_payload, auth=True
        )
        assert avail_resp.status_code == 200
        data = avail_resp.json()
        # Both tasks contribute to busy_hours in the overlap window
        assert data["busy_hours"] > 0, "Cross-project busy_hours must be > 0 in overlap"

    def test_task_schedule_updated_when_dates_change(self, registered_user, client):
        """Updating task dates refreshes task_schedule entries"""
        worker = self._make_user(client, "date_update_worker")

        task_data = {
            "title": "Date Update Task",
            "description": "Dates will change",
            "start_date": "2026-08-01",
            "due_date": "2026-08-05",
            "assignee_user_id": registered_user.user_id,
            "estimated_hours": 20,
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        registered_user.post(
            f"/tasks/{task_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 20.0},
            auth=True,
        )

        # Old range should show busy_hours
        avail_old = registered_user.post(
            "/tasks/availability/calculate",
            {
                "user_id": worker.user_id,
                "start_ts": "2026-08-01T00:00:00Z",
                "end_ts": "2026-08-05T23:59:59Z",
            },
            auth=True,
        )
        assert avail_old.json()["busy_hours"] > 0

        # Update task to new dates
        update_resp = registered_user.put(
            f"/tasks/{task_id}",
            {"start_date": "2026-09-01", "due_date": "2026-09-05"},
            auth=True,
        )
        assert update_resp.status_code == 200

        # Old range should now show 0 busy_hours
        avail_old_after = registered_user.post(
            "/tasks/availability/calculate",
            {
                "user_id": worker.user_id,
                "start_ts": "2026-08-01T00:00:00Z",
                "end_ts": "2026-08-05T23:59:59Z",
            },
            auth=True,
        )
        assert avail_old_after.json()["busy_hours"] == 0.0

        # New range should show busy_hours
        avail_new = registered_user.post(
            "/tasks/availability/calculate",
            {
                "user_id": worker.user_id,
                "start_ts": "2026-09-01T00:00:00Z",
                "end_ts": "2026-09-05T23:59:59Z",
            },
            auth=True,
        )
        assert avail_new.json()["busy_hours"] > 0

    def test_supervisor_can_read_member_workload(self, registered_user, client):
        """Project supervisor/owner can read a member's workload (cross-project access)"""
        worker = self._make_user(client, "workload_member")

        task_data = {
            "title": "Workload Project Task",
            "description": "Supervisor checks member workload",
            "start_date": "2026-09-01",
            "due_date": "2026-09-30",
            "assignee_user_id": registered_user.user_id,
            "estimated_hours": 20,
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        assert task_resp.status_code == 201, f"Task creation failed: {task_resp.text}"
        task_id = task_resp.json()["id"]

        registered_user.post(
            f"/tasks/{task_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 80.0},
            auth=True,
        )

        # Registered user (owner) reads worker's workload
        workload_resp = registered_user.get(
            f"/users/{worker.user_id}/workload",
            params={
                "start_ts": "2026-09-01T00:00:00Z",
                "end_ts": "2026-09-30T23:59:59Z",
            },
            auth=True,
        )
        assert workload_resp.status_code == 200, (
            f"Owner should be able to read member workload, got {workload_resp.status_code}: {workload_resp.text}"
        )
        data = workload_resp.json()
        assert data["user_id"] == worker.user_id
        assert data["scheduled_tasks"] >= 1

    def test_unrelated_user_cannot_read_workload(self, registered_user, client):
        """User with no shared project cannot read another user's workload"""
        worker = self._make_user(client, "workload_isolated")

        response = registered_user.get(
            f"/users/{worker.user_id}/workload",
            params={
                "start_ts": "2026-09-01T00:00:00Z",
                "end_ts": "2026-09-30T23:59:59Z",
            },
            auth=True,
        )
        # No shared project → 403
        assert response.status_code == 403


class TestRoleChangeWorkflow:
    """Role change by delete + re-assign (frontend pattern from AssignmentManager)"""

    @staticmethod
    def _make_user(client: "APIClient", prefix: str) -> "APIClient":
        import uuid

        email = f"{prefix}_{uuid.uuid4().hex[:8]}@example.com"
        payload = {
            "email": email,
            "password": "TestPass123!",
            "display_name": f"{prefix} User",
            "work_schedule": [],
        }
        r = client.post("/auth/register", payload)
        assert r.status_code == 201
        data = r.json()
        c = APIClient()
        c.set_token(data["token"], data["user"]["id"])
        return c

    def test_change_role_via_delete_and_reassign(self, registered_user, client):
        """Role change: delete assignment → re-assign with new role → verify"""
        member = self._make_user(client, "role_change")

        task_data = {"title": "Role Change Project", "description": "Role swap test"}
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        assert task_resp.status_code == 201
        task_id = task_resp.json()["id"]

        # Assign as executor
        assign_resp = registered_user.post(
            f"/tasks/{task_id}/assignments",
            {"user_id": member.user_id, "role": "executor"},
            auth=True,
        )
        assert assign_resp.status_code == 201

        # Verify role is executor
        list_resp = registered_user.get(f"/tasks/{task_id}/assignments", auth=True)
        roles = {a["user_id"]: a.get("role") for a in list_resp.json()}
        assert roles.get(member.user_id) == "executor"

        # Delete the assignment
        del_resp = registered_user.delete(
            f"/tasks/{task_id}/assignments/{member.user_id}", auth=True
        )
        assert del_resp.status_code == 200

        # Re-assign with supervisor role
        reassign_resp = registered_user.post(
            f"/tasks/{task_id}/assignments",
            {"user_id": member.user_id, "role": "supervisor"},
            auth=True,
        )
        assert reassign_resp.status_code == 201

        # Verify new role is supervisor
        list_resp2 = registered_user.get(f"/tasks/{task_id}/assignments", auth=True)
        roles2 = {a["user_id"]: a.get("role") for a in list_resp2.json()}
        assert roles2.get(member.user_id) == "supervisor"

    def test_owner_cannot_be_removed(self, registered_user, client):
        """Attempting to remove project owner returns an appropriate error or ignores"""
        task_data = {
            "title": "Owner Keep Project",
            "description": "Should not remove owner",
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        # Try to remove self (who is owner) — backend may forbid or allow
        del_resp = registered_user.delete(
            f"/tasks/{task_id}/assignments/{registered_user.user_id}", auth=True
        )
        # Either 200 (allowed) or 403 (forbidden) are valid behaviors
        assert del_resp.status_code in (200, 403, 400)

    def test_spectator_role_assignment(self, registered_user, client):
        """User can be assigned spectator role"""
        spectator = self._make_user(client, "spectator")

        task_data = {"title": "Spectator Task", "description": "Read only member"}
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        assign_resp = registered_user.post(
            f"/tasks/{task_id}/assignments",
            {"user_id": spectator.user_id, "role": "spectator"},
            auth=True,
        )
        assert assign_resp.status_code == 201

        list_resp = registered_user.get(f"/tasks/{task_id}/assignments", auth=True)
        roles = {a["user_id"]: a.get("role") for a in list_resp.json()}
        assert roles.get(spectator.user_id) == "spectator"


class TestSchedulingConflictDetection:
    """
    Conflict detection: POST /tasks/{id}/assignments returns scheduling_conflicts
    when the user is already assigned to an overlapping task.
    Also verifies the end-of-day inclusive boundary (due_date covers the full day).
    """

    @staticmethod
    def _make_user(client: "APIClient", prefix: str) -> "APIClient":
        import uuid

        email = f"{prefix}_{uuid.uuid4().hex[:8]}@example.com"
        payload = {
            "email": email,
            "password": "TestPass123!",
            "display_name": f"{prefix} User",
            "work_schedule": [],
        }
        r = client.post("/auth/register", payload)
        assert r.status_code == 201
        data = r.json()
        c = APIClient()
        c.set_token(data["token"], data["user"]["id"])
        return c

    def test_conflict_returned_when_dates_overlap(self, registered_user, client):
        """Assigning a user to two tasks with overlapping dates returns scheduling_conflicts"""
        worker = self._make_user(client, "conflict_worker")

        # Task 1: July 1–10
        t1_resp = registered_user.post(
            "/tasks",
            {
                "title": "Conflict Task A",
                "description": "First booking",
                "start_date": "2026-07-01",
                "due_date": "2026-07-10",
                "assignee_user_id": registered_user.user_id,
                "estimated_hours": 40,
            },
            auth=True,
        )
        assert t1_resp.status_code == 201
        task1_id = t1_resp.json()["id"]

        # Assign worker to task 1
        r1 = registered_user.post(
            f"/tasks/{task1_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 40.0},
            auth=True,
        )
        assert r1.status_code == 201
        assert "scheduling_conflicts" not in r1.json(), (
            "First assignment should have no conflicts"
        )

        # Task 2: July 5–15 — overlaps with task 1
        t2_resp = registered_user.post(
            "/tasks",
            {
                "title": "Conflict Task B",
                "description": "Second booking overlaps",
                "start_date": "2026-07-05",
                "due_date": "2026-07-15",
                "assignee_user_id": registered_user.user_id,
                "estimated_hours": 40,
            },
            auth=True,
        )
        assert t2_resp.status_code == 201
        task2_id = t2_resp.json()["id"]

        # Assign worker to task 2 — should report scheduling_conflicts
        r2 = registered_user.post(
            f"/tasks/{task2_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 40.0},
            auth=True,
        )
        assert r2.status_code == 201, (
            f"Assignment should succeed (warning, not block): {r2.text}"
        )
        body = r2.json()
        assert "scheduling_conflicts" in body, (
            f"Expected scheduling_conflicts in response when overlap exists, got: {body}"
        )
        conflicts = body["scheduling_conflicts"]
        assert len(conflicts) >= 1
        conflict_task_ids = [c["task_id"] for c in conflicts]
        assert task1_id in conflict_task_ids, "Task 1 should appear in conflicts"

    def test_no_conflict_when_tasks_do_not_overlap(self, registered_user, client):
        """Assigning a user to non-overlapping tasks returns no scheduling_conflicts"""
        worker = self._make_user(client, "no_conflict_worker")

        # Task 1: July 1–10
        t1_resp = registered_user.post(
            "/tasks",
            {
                "title": "No Conflict A",
                "description": "First booking",
                "start_date": "2026-07-01",
                "due_date": "2026-07-10",
                "assignee_user_id": registered_user.user_id,
                "estimated_hours": 20,
            },
            auth=True,
        )
        task1_id = t1_resp.json()["id"]
        registered_user.post(
            f"/tasks/{task1_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 20.0},
            auth=True,
        )

        # Task 2: July 11–20 — starts exactly when task 1 ends (next day)
        t2_resp = registered_user.post(
            "/tasks",
            {
                "title": "No Conflict B",
                "description": "Second booking after first",
                "start_date": "2026-07-11",
                "due_date": "2026-07-20",
                "assignee_user_id": registered_user.user_id,
                "estimated_hours": 20,
            },
            auth=True,
        )
        task2_id = t2_resp.json()["id"]

        r2 = registered_user.post(
            f"/tasks/{task2_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 20.0},
            auth=True,
        )
        assert r2.status_code == 201
        body = r2.json()
        assert "scheduling_conflicts" not in body, (
            f"Expected no scheduling_conflicts for non-overlapping tasks, got: {body}"
        )

    def test_due_date_end_of_day_inclusive(self, registered_user, client):
        """Task with due_date=July 10 should include the full day (end_ts = July 10 23:59:59).
        A task starting July 10 must conflict with it."""
        worker = self._make_user(client, "eod_worker")

        # Task 1: July 1–10 inclusive
        t1_resp = registered_user.post(
            "/tasks",
            {
                "title": "EOD Task A",
                "description": "End of day test task A",
                "start_date": "2026-07-01",
                "due_date": "2026-07-10",
                "assignee_user_id": registered_user.user_id,
                "estimated_hours": 40,
            },
            auth=True,
        )
        assert t1_resp.status_code == 201, f"Task A creation failed: {t1_resp.text}"
        task1_id = t1_resp.json()["id"]
        registered_user.post(
            f"/tasks/{task1_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 40.0},
            auth=True,
        )

        # Task 2: starts July 10 (same day as task 1 ends) — must detect overlap
        t2_resp = registered_user.post(
            "/tasks",
            {
                "title": "EOD Task B",
                "description": "End of day test task B",
                "start_date": "2026-07-10",
                "due_date": "2026-07-12",
                "assignee_user_id": registered_user.user_id,
                "estimated_hours": 16,
            },
            auth=True,
        )
        assert t2_resp.status_code == 201, f"Task B creation failed: {t2_resp.text}"
        task2_id = t2_resp.json()["id"]

        r2 = registered_user.post(
            f"/tasks/{task2_id}/assignments",
            {"user_id": worker.user_id, "assigned_hours": 16.0},
            auth=True,
        )
        assert r2.status_code == 201
        body = r2.json()
        assert "scheduling_conflicts" in body, (
            "Task starting on the same day as another task's due_date must report a conflict "
            "(end_ts is end of July 10, start_ts of new task is start of July 10)"
        )

    def test_conflict_also_visible_in_availability(self, registered_user, client):
        """After conflicting assignment, availability endpoint shows busy_hours from both tasks"""
        worker = self._make_user(client, "avail_conflict_worker")

        # Two tasks in the same July 5–10 window
        for title, start, end, hours in [
            ("Availability Conflict A", "2026-07-01", "2026-07-10", 40.0),
            ("Availability Conflict B", "2026-07-05", "2026-07-15", 40.0),
        ]:
            t_resp = registered_user.post(
                "/tasks",
                {
                    "title": title,
                    "description": f"Availability conflict test: {title}",
                    "start_date": start,
                    "due_date": end,
                    "assignee_user_id": registered_user.user_id,
                    "estimated_hours": int(hours),
                },
                auth=True,
            )
            assert t_resp.status_code == 201, f"Task creation failed: {t_resp.text}"
            task_id = t_resp.json()["id"]
            registered_user.post(
                f"/tasks/{task_id}/assignments",
                {"user_id": worker.user_id, "assigned_hours": hours},
                auth=True,
            )

        avail = registered_user.post(
            "/tasks/availability/calculate",
            {
                "user_id": worker.user_id,
                "start_ts": "2026-07-05T00:00:00Z",
                "end_ts": "2026-07-10T23:59:59Z",
            },
            auth=True,
        )
        assert avail.status_code == 200
        assert avail.json()["busy_hours"] > 0


if __name__ == "__main__":
    pytest.main([__file__, "-v", "--tb=short"])
