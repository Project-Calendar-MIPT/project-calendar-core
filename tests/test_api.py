"""
Integration tests for Project Calendar API
Tests cover authentication, task management, and calendar functionality
"""

import pytest
import requests
import time
from typing import Dict, Optional
import os
from pathlib import Path
import yaml

# Configuration
BASE_URL = os.getenv("TEST_BASE_URL", "http://localhost:8081")
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
        response = requests.get(url, params=params, headers=self.headers() if auth else {})
        return response
    
    def put(self, endpoint: str, data: dict, auth: bool = True):
        """PUT request"""
        url = f"{self.base_url}{API_PREFIX}{endpoint}"
        response = requests.put(url, json=data, headers=self.headers() if auth else {})
        return response
    
    def delete(self, endpoint: str, auth: bool = True):
        """DELETE request"""
        url = f"{self.base_url}{API_PREFIX}{endpoint}"
        response = requests.delete(url, headers=self.headers() if auth else {})
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
                print(f"Backend is ready after {i+1} attempts")
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
            {"weekday": 4, "start_time": "09:00:00", "end_time": "18:00:00"}
        ]
    }
    
    response = client.post("/auth/register", user_data)
    assert response.status_code == 201, f"Registration failed: {response.text}"
    
    data = response.json()
    assert "token" in data
    assert "user" in data
    
    client.set_token(data["token"], data["user"]["id"])
    return client


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
            ]
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
            ]
        }
        
        response = registered_user.post("/auth/register", user_data)
        assert response.status_code == 409  # Conflict
    
    def test_register_invalid_password(self, client):
        """Test registration with empty password is rejected"""
        import uuid
        user_data = {
            "email": f"test_{uuid.uuid4().hex[:8]}@example.com",
            "password": "",
            "display_name": "Test",
            "work_schedule": [
                {"weekday": 0, "start_time": "09:00:00", "end_time": "17:00:00"}
            ]
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
            ]
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
            "estimated_hours": 10.5
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
            "parent_task_id": parent_id
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
        update_data = {
            "title": "Updated Title",
            "status": "in_progress"
        }
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
            f"/tasks/{task_id}/assignments",
            assignment_data,
            auth=True
        )
        # Should get 409 Conflict because owner is already assigned
        assert response.status_code == 409
    
    def test_list_assignments(self, registered_user):
        """Test listing task assignments"""
        # Create a task with assignment
        task_data = {"title": "Task for Assignment List", "description": "Assignment list test"}
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
                "assignee_user_id": registered_user.user_id
            }
            registered_user.post("/tasks", task_data, auth=True)
        
        # Get calendar tasks
        params = {
            "start_date": "2024-01-01",
            "end_date": "2024-01-31"
        }
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
            "estimated_hours": -5
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_create_task_zero_hours(self, registered_user):
        """Creating a task with 0 estimated_hours should succeed"""
        task_data = {
            "title": "Zero Hours",
            "description": "Test description",
            "estimated_hours": 0
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 201

    def test_create_task_valid(self, registered_user):
        """Creating a task with valid description and hours"""
        task_data = {
            "title": "Valid Task",
            "description": "This task has a proper description",
            "estimated_hours": 8.5
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 201
        data = response.json()
        assert data["title"] == "Valid Task"
        assert data["description"] == "This task has a proper description"

    def test_update_task_empty_description(self, registered_user):
        """Updating task description to empty should fail"""
        task_data = {
            "title": "Task for Update",
            "description": "Original description"
        }
        create_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = create_resp.json()["id"]

        update_data = {"description": ""}
        response = registered_user.put(f"/tasks/{task_id}", update_data, auth=True)
        assert response.status_code == 400

    def test_update_task_negative_hours(self, registered_user):
        """Updating estimated_hours to negative should fail"""
        task_data = {
            "title": "Task for Hour Update",
            "description": "Some description"
        }
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
            "assignee_user_id": registered_user.user_id
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
            "assignee_user_id": registered_user.user_id
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
            "assignee_user_id": registered_user.user_id
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        project_id = proj_resp.json()["id"]

        task_data = {
            "title": "Early Task",
            "description": "Task starting too early",
            "project_root_id": project_id,
            "start_date": "2025-01-01",
            "due_date": "2025-07-01"
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
            "assignee_user_id": registered_user.user_id
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        project_id = proj_resp.json()["id"]

        task_data = {
            "title": "Late Task",
            "description": "Task ending too late",
            "project_root_id": project_id,
            "start_date": "2025-03-01",
            "due_date": "2025-12-31"
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
            "assignee_user_id": registered_user.user_id
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        project_id = proj_resp.json()["id"]

        task_data = {
            "title": "Task to Update Dates",
            "description": "Proper description",
            "project_root_id": project_id,
            "start_date": "2025-02-01",
            "due_date": "2025-05-01",
            "assignee_user_id": registered_user.user_id
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
        parent_data = {
            "title": "Parent Task",
            "description": "Parent description"
        }
        parent_resp = registered_user.post("/tasks", parent_data, auth=True)
        assert parent_resp.status_code == 201
        parent_id = parent_resp.json()["id"]

        subtask_data = {
            "title": "Subtask",
            "description": "Subtask description",
            "parent_task_id": parent_id
        }
        response = registered_user.post("/tasks", subtask_data, auth=True)
        assert response.status_code == 201
        assert response.json()["parent_task_id"] == parent_id

    def test_get_subtasks(self, registered_user):
        """List subtasks of a parent"""
        parent_data = {
            "title": "Parent for List",
            "description": "Parent description"
        }
        parent_resp = registered_user.post("/tasks", parent_data, auth=True)
        parent_id = parent_resp.json()["id"]

        for i in range(3):
            sub_data = {
                "title": f"Subtask {i}",
                "description": f"Sub description {i}",
                "parent_task_id": parent_id
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
            "description": "Parent description"
        }
        parent_resp = registered_user.post("/tasks", parent_data, auth=True)
        parent_id = parent_resp.json()["id"]

        sub_data = {
            "title": "Subtask to Update",
            "description": "Original sub description",
            "parent_task_id": parent_id
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
            "description": "Parent description"
        }
        parent_resp = registered_user.post("/tasks", parent_data, auth=True)
        parent_id = parent_resp.json()["id"]

        sub_data = {
            "title": "Subtask to Delete",
            "description": "Sub description",
            "parent_task_id": parent_id
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
            "parent_task_id": p1_id
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
            "parent_task_id": fake_id
        }
        response = registered_user.post("/tasks", sub_data, auth=True)
        assert response.status_code == 400


class TestTaskNotes:
    """Test task notes CRUD"""

    def test_create_note(self, registered_user):
        """Create a note on a task"""
        task_data = {
            "title": "Task with Notes",
            "description": "Task for notes test"
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        note_data = {"content": "This is a test note"}
        response = registered_user.post(
            f"/tasks/{task_id}/notes", note_data, auth=True
        )
        assert response.status_code == 201
        data = response.json()
        assert data["content"] == "This is a test note"
        assert data["task_id"] == task_id
        assert "id" in data
        assert "created_at" in data

    def test_create_note_empty_content(self, registered_user):
        """Creating a note with empty content should fail"""
        task_data = {
            "title": "Task for Empty Note",
            "description": "Description"
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        note_data = {"content": ""}
        response = registered_user.post(
            f"/tasks/{task_id}/notes", note_data, auth=True
        )
        assert response.status_code == 400

    def test_list_notes(self, registered_user):
        """List notes on a task"""
        task_data = {
            "title": "Task for Listing Notes",
            "description": "Description"
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        for i in range(3):
            registered_user.post(
                f"/tasks/{task_id}/notes",
                {"content": f"Note {i}"},
                auth=True
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
        task_data = {
            "title": "Task for Note Update",
            "description": "Description"
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        note_resp = registered_user.post(
            f"/tasks/{task_id}/notes",
            {"content": "Original note"},
            auth=True
        )
        note_id = note_resp.json()["id"]

        update_data = {"content": "Updated note content"}
        response = registered_user.put(f"/notes/{note_id}", update_data, auth=True)
        assert response.status_code == 200
        assert response.json()["content"] == "Updated note content"

    def test_delete_note(self, registered_user):
        """Delete a note"""
        task_data = {
            "title": "Task for Note Delete",
            "description": "Description"
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        note_resp = registered_user.post(
            f"/tasks/{task_id}/notes",
            {"content": "Note to delete"},
            auth=True
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
            "assignee_user_id": registered_user.user_id
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
            "due_date": "2025-05-01"
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
            "assignee_user_id": registered_user.user_id
        }
        proj_resp = registered_user.post("/tasks", project_data, auth=True)
        proj_id = proj_resp.json()["id"]

        task_data = {
            "title": "Unassigned Task",
            "description": "Task without project"
        }
        task_resp = registered_user.post("/tasks", task_data, auth=True)
        task_id = task_resp.json()["id"]

        update_data = {
            "project_root_id": proj_id,
            "start_date": "2025-03-01",
            "due_date": "2025-06-01"
        }
        response = registered_user.put(f"/tasks/{task_id}", update_data, auth=True)
        assert response.status_code == 200
        assert response.json()["project_root_id"] == proj_id

    def test_set_parent_task_id_to_null(self, registered_user):
        """Detach subtask from parent via PUT"""
        parent_data = {
            "title": "Parent for Detach",
            "description": "Parent desc"
        }
        parent_resp = registered_user.post("/tasks", parent_data, auth=True)
        parent_id = parent_resp.json()["id"]

        sub_data = {
            "title": "Subtask for Detach",
            "description": "Sub desc",
            "parent_task_id": parent_id
        }
        sub_resp = registered_user.post("/tasks", sub_data, auth=True)
        sub_id = sub_resp.json()["id"]

        update_data = {"parent_task_id": None}
        response = registered_user.put(f"/tasks/{sub_id}", update_data, auth=True)
        assert response.status_code == 200
        assert response.json()["parent_task_id"] is None

    def test_update_self_parent_rejected(self, registered_user):
        """Setting task as its own parent should fail"""
        task_data = {
            "title": "Self Parent",
            "description": "Test self reference"
        }
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
            "due_date": "2026-03-16"
        }
        response = registered_user.post("/tasks", task_data, auth=True)
        assert response.status_code == 400

    def test_task_with_dates_auto_status_in_progress(self, registered_user):
        task_data = {
            "title": "Timed task auto status",
            "description": "Auto in progress",
            "start_date": "2026-03-14",
            "due_date": "2026-03-16",
            "assignee_user_id": registered_user.user_id
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
            "assignee_user_id": registered_user.user_id
        }
        plain = {
            "title": "Untimed for filter",
            "description": "No dates"
        }
        r1 = registered_user.post("/tasks", timed, auth=True)
        r2 = registered_user.post("/tasks", plain, auth=True)
        assert r1.status_code == 201
        assert r2.status_code == 201
        timed_id = r1.json()["id"]
        plain_id = r2.json()["id"]

        with_time = registered_user.get("/tasks", params={"has_time": "true"}, auth=True)
        assert with_time.status_code == 200
        ids_with_time = {x.get("id") for x in with_time.json()}
        assert timed_id in ids_with_time

        without_time = registered_user.get("/tasks", params={"has_time": "false"}, auth=True)
        assert without_time.status_code == 200
        ids_without_time = {x.get("id") for x in without_time.json()}
        assert plain_id in ids_without_time

    def test_calculate_availability_endpoint(self, registered_user):
        payload = {
            "user_id": registered_user.user_id,
            "start_ts": "2026-03-14T09:00:00Z",
            "end_ts": "2026-03-14T18:00:00Z"
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
            "assignee_user_id": registered_user.user_id
        }
        create_resp = registered_user.post("/tasks", task_data, auth=True)
        assert create_resp.status_code == 201
        task_id = create_resp.json()["id"]

        payload = {
            "required_skills": ["backend", "sql"],
            "start_ts": "2026-03-18T09:00:00Z",
            "end_ts": "2026-03-19T18:00:00Z"
        }
        response = registered_user.post(
            f"/tasks/{task_id}/candidate-assignees", payload, auth=True
        )
        assert response.status_code == 200
        data = response.json()
        assert data["task_id"] == task_id
        assert isinstance(data["candidates"], list)

    def test_user_workload_endpoint(self, registered_user):
        params = {
            "start_ts": "2026-03-14T00:00:00Z",
            "end_ts": "2026-03-21T00:00:00Z"
        }
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


if __name__ == "__main__":
    pytest.main([__file__, "-v", "--tb=short"])
